class_name ObjectEditorWorkspace
extends "res://modtools/editor/editor_workspace.gd"

const ObjectEditorScript = preload("res://modtools/object/object_editor.gd")
const ObjectPreviewScript = preload("res://modtools/object/object_preview.gd")
const ShaderTagPickerScript = preload("res://modtools/object/ui/widgets/shader_tag_picker.gd")
const TextureSlotWidgetScript = preload("res://modtools/object/ui/widgets/texture_slot_widget.gd")
const CtrlRegPickerScript = preload("res://modtools/object/ui/widgets/ctrl_reg_picker.gd")
const AnimFramesDialogScript = preload("res://modtools/object/ui/dialogs/anim_frames_dialog.gd")

enum Workflow { PREVIEW, MATERIALS, PARTS, LIGHTS, LODS }

const WORKFLOW_DEFS := [
	{"id": Workflow.PREVIEW, "label": "Preview", "tooltip": "View the object with fixed editor lighting."},
	{"id": Workflow.MATERIALS, "label": "Materials", "tooltip": "Edit material shader tags, textures, and alpha."},
	{"id": Workflow.PARTS, "label": "Part Anims", "tooltip": "Inspect and edit PANM part animation entries."},
	{"id": Workflow.LIGHTS, "label": "Lights", "tooltip": "Inspect and edit object light colors."},
	{"id": Workflow.LODS, "label": "LODs", "tooltip": "Bind ASE scenes and edit project LOD settings."},
]

const TEXTURE_SLOTS := [
	{"slot": 1, "label": "Diffuse"},
	{"slot": 2, "label": "Detail"},
	{"slot": 3, "label": "Normal A"},
	{"slot": 4, "label": "Normal B"},
]
const OED_UPDATE_NONE := 0
const OED_UPDATE_MTRL := 1
const OED_UPDATE_LGHT := 2
const OED_UPDATE_PANM := 4
const OED_UPDATE_ALL := OED_UPDATE_MTRL | OED_UPDATE_LGHT | OED_UPDATE_PANM
const GENERATOR_STYLE_OPTIONS := [
	{"id": 0, "label": "None"},
	{"id": 16, "label": "Slide"},
	{"id": 17, "label": "Slide inverse"},
	{"id": 24, "label": "Set"},
	{"id": 32, "label": "Rotate CW"},
	{"id": 33, "label": "Rotate CCW"},
	{"id": 49, "label": "Set wave sine"},
	{"id": 50, "label": "Set wave square"},
	{"id": 51, "label": "Set wave triangle"},
	{"id": 52, "label": "Set wave saw"},
	{"id": 65, "label": "Add wave sine"},
	{"id": 81, "label": "Skew wave sine"},
	{"id": 97, "label": "Multiply wave sine"},
	{"id": 113, "label": "Control register set"},
	{"id": 114, "label": "Control register add"},
]
const PART_ANIM_SCALE_OPTIONS := [
	{"id": 0, "label": "None"},
	{"id": 1, "label": "Uniform"},
	{"id": 2, "label": "Per-axis"},
]
const PART_ANIM_ROTATION_OPTIONS := [
	{"id": 0, "label": "None"},
	{"id": 1, "label": "Spinner"},
	{"id": 2, "label": "Euler"},
	{"id": 3, "label": "Face camera"},
	{"id": 4, "label": "Face camera XZ"},
]
const PART_ANIM_TRANSLATE_OPTIONS := [
	{"id": 0, "label": "None"},
	{"id": 1, "label": "X axis"},
	{"id": 2, "label": "Y axis"},
	{"id": 3, "label": "Z axis"},
]
const PART_ANIM_TRACK_DEFS := [
	{"id": "rotation_x", "label": "Rotation X"},
	{"id": "rotation_y", "label": "Rotation Y"},
	{"id": "rotation_z", "label": "Rotation Z"},
	{"id": "scale_x", "label": "Scale X"},
	{"id": "scale_y", "label": "Scale Y"},
	{"id": "scale_z", "label": "Scale Z"},
	{"id": "translation", "label": "Translation"},
]
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

var object_editor: ObjectEditor
var environment_editor
var _active_workflow_id: int = Workflow.PREVIEW
var _preview: ObjectPreview
var _asset_dock_host: Control
var _object_detail_dock: Control
var _material_clipboard: Dictionary = {}
var _export_update_mask: int = OED_UPDATE_NONE
var _material_selected_index := 0
var _materials_list: ItemList
var _material_paste_button: Button
var _light_selected_index := 0
var _lights_list: ItemList
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
		if not _preview.environment_button_pressed.is_connected(_on_preview_environment_button_pressed):
			_preview.environment_button_pressed.connect(_on_preview_environment_button_pressed)
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
	return true


func set_asset_dock(dock: Control) -> void:
	if _object_detail_dock != null and _object_detail_dock.get_parent() != null:
		_object_detail_dock.get_parent().remove_child(_object_detail_dock)
	if dock == null:
		if _object_detail_dock != null:
			_object_detail_dock.free()
			_object_detail_dock = null
		_asset_dock_host = null
		return
	_asset_dock_host = dock
	if _object_detail_dock == null:
		_object_detail_dock = PanelContainer.new()
		_object_detail_dock.name = "ObjectDetailDock"
		_object_detail_dock.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_object_detail_dock.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_asset_dock_host.add_child(_object_detail_dock)
	_rebuild_object_detail_dock()


func sync_asset_dock() -> void:
	_rebuild_object_detail_dock()


func get_workflows() -> Array:
	return WORKFLOW_DEFS


func get_active_workflow_id() -> int:
	return _active_workflow_id


func activate_workflow(workflow_id: int) -> void:
	_active_workflow_id = workflow_id
	_rebuild_object_detail_dock()


func build_workflow_inspector(workflow_id: int, host: Control) -> void:
	_ensure_object_editor()
	_active_workflow_id = workflow_id
	match workflow_id:
		Workflow.MATERIALS:
			_build_materials_inspector(host)
		Workflow.PARTS:
			_build_part_anims_inspector(host)
		Workflow.LIGHTS:
			_build_lights_inspector(host)
		Workflow.LODS:
			_build_lods_inspector(host)
		_:
			_build_preview_inspector(host)
	_rebuild_object_detail_dock()


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
	_build_preview_inspector(host)


func _ensure_object_editor() -> void:
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
	elif _active_workflow_id == Workflow.MATERIALS and _materials_list != null:
		_refresh_materials_left_list()
	elif _active_workflow_id == Workflow.LIGHTS and _lights_list != null:
		_refresh_lights_left_list()
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


func _on_preview_environment_button_pressed() -> void:
	if editor_shell != null and editor_shell.has_method("show_environment_dialog"):
		editor_shell.show_environment_dialog()


func _build_preview_inspector(host: Control) -> void:
	var box := _make_inspector_box(host)
	var summary := object_editor.object_data.get_summary() if object_editor and object_editor.object_data else {}
	for key in ["source_kind", "lod_count", "material_count", "light_count", "userpoint_count"]:
		var row := Label.new()
		row.text = "%s: %s" % [String(key).capitalize(), str(summary.get(key, ""))]
		row.clip_text = true
		box.add_child(row)

	var playback := HBoxContainer.new()
	playback.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(playback)

	var play := Button.new()
	play.name = "PreviewPlayButton"
	play.toggle_mode = true
	play.button_pressed = _preview == null or _preview.is_playing()
	play.text = "Pause" if play.button_pressed else "Play"
	playback.add_child(play)

	var reset := Button.new()
	reset.name = "PreviewResetButton"
	reset.text = "Reset"
	playback.add_child(reset)

	var wire := CheckBox.new()
	wire.name = "PreviewWireCheck"
	wire.text = "Wire"
	wire.button_pressed = _preview != null and _preview.is_wireframe()
	playback.add_child(wire)

	play.toggled.connect(func(pressed: bool) -> void:
		if _preview != null:
			_preview.set_playing(pressed)
		play.text = "Pause" if pressed else "Play"
	)
	reset.pressed.connect(func() -> void:
		if _preview != null:
			_preview.reset_animation_time()
	)
	wire.toggled.connect(func(pressed: bool) -> void:
		if _preview != null:
			_preview.set_wireframe(pressed)
	)

	_build_export_mask_controls(box)

	var ctrl_regs: Array = object_editor.object_data.get_control_registers() if object_editor and object_editor.object_data and object_editor.object_data.has_method("get_control_registers") else []
	if not ctrl_regs.is_empty() and _preview != null:
		var ctrl_label := Label.new()
		ctrl_label.text = "Control registers"
		box.add_child(ctrl_label)
		for reg in ctrl_regs:
			var reg_name := String((reg as Dictionary).get("name", ""))
			if reg_name.is_empty():
				continue
			var row := HBoxContainer.new()
			row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			box.add_child(row)
			var label := Label.new()
			label.text = reg_name
			label.custom_minimum_size = Vector2(90, 0)
			row.add_child(label)
			var slider := HSlider.new()
			slider.name = "ControlRegisterSlider_%s" % reg_name.replace(" ", "_")
			slider.min_value = 0
			slider.max_value = 65535
			slider.step = 1
			slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			row.add_child(slider)
			slider.value_changed.connect(func(value: float, name: String = reg_name) -> void:
				if _preview != null:
					_preview.set_ctrl_value(name, int(value))
			)


func _build_export_mask_controls(box: VBoxContainer) -> void:
	if object_editor == null or object_editor.object_data == null or not object_editor.object_data.can_export_3di():
		return
	_sync_export_update_mask_from_dirty()
	var label := Label.new()
	label.text = "Export chunks"
	box.add_child(label)

	var row := HBoxContainer.new()
	row.name = "ObjectExportMaskControls"
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(row)

	_add_export_mask_check(row, "ObjectExportMtrlCheck", "MTRL", OED_UPDATE_MTRL)
	_add_export_mask_check(row, "ObjectExportLghtCheck", "LGHT", OED_UPDATE_LGHT)
	_add_export_mask_check(row, "ObjectExportPanmCheck", "PANM", OED_UPDATE_PANM)


func _add_export_mask_check(parent: HBoxContainer, node_name: String, label: String, bit: int) -> CheckBox:
	var check := CheckBox.new()
	check.name = node_name
	check.text = label
	check.button_pressed = (_export_update_mask & bit) != 0
	parent.add_child(check)
	check.toggled.connect(func(pressed: bool) -> void:
		if pressed:
			_export_update_mask |= bit
		else:
			_export_update_mask &= ~bit
	)
	return check


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


func _build_lods_inspector(host: Control) -> void:
	var box := _make_inspector_box(host)
	var data: NovaObjectData = object_editor.object_data if object_editor else null
	var summary: Dictionary = data.get_summary() if data != null else {}
	var lods: Array = data.get_project_lods() if data != null and data.has_method("get_project_lods") else []
	var selected := {"index": 0 if not lods.is_empty() else -1}
	var syncing := {"value": false}

	var list := ItemList.new()
	list.name = "ObjectLodsList"
	list.custom_minimum_size = Vector2(0, 150)
	list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for lod in lods:
		var info := lod as Dictionary
		list.add_item("LOD %d  %s" % [int(info.get("index", 0)), String(info.get("scene_file", ""))])
	box.add_child(list)

	var action_row := HBoxContainer.new()
	action_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(action_row)
	var add_button := Button.new()
	add_button.name = "ObjectAddLodSceneButton"
	add_button.text = "Add ASE"
	action_row.add_child(add_button)
	var replace_button := Button.new()
	replace_button.name = "ObjectReplaceLodSceneButton"
	replace_button.text = "Replace ASE"
	replace_button.disabled = selected["index"] < 0
	action_row.add_child(replace_button)

	var scene_dialog := FileDialog.new()
	scene_dialog.name = "ObjectLodSceneFileDialog"
	scene_dialog.access = FileDialog.ACCESS_FILESYSTEM
	scene_dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	scene_dialog.filters = PackedStringArray(["*.ase,*.ASE ; ASE scene"])
	box.add_child(scene_dialog)

	var poly_lod := _add_spin_row(box, "ObjectPolyCollisionLod", "Collision LOD", 0, 7, 1)
	_set_spin(poly_lod, int(summary.get("poly_collision_lod", 0)))
	var threshold := _add_spin_row(box, "ObjectLodThreshold", "Threshold", 0, 100000, 0.1)
	var attributes := _add_spin_row(box, "ObjectLodAttributes", "Attributes", 0, 255, 1)
	var render_row := HBoxContainer.new()
	render_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(render_row)
	var render_label := Label.new()
	render_label.text = "Render fn"
	render_label.custom_minimum_size = Vector2(90, 0)
	render_row.add_child(render_label)
	var render_function := LineEdit.new()
	render_function.name = "ObjectLodRenderFunction"
	render_function.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	render_row.add_child(render_function)

	var set_lod_controls_enabled := func(enabled: bool) -> void:
		threshold.editable = enabled
		attributes.editable = enabled
		render_function.editable = enabled
		replace_button.disabled = not enabled

	var sync := func(index: int) -> void:
		syncing["value"] = true
		selected["index"] = index
		if index >= 0 and index < lods.size():
			var info := lods[index] as Dictionary
			_set_spin(threshold, float(info.get("threshold", 0.0)))
			_set_spin(attributes, int(info.get("attributes", 0)))
			render_function.text = String(info.get("render_function", ""))
			set_lod_controls_enabled.call(true)
		else:
			_set_spin(threshold, 0)
			_set_spin(attributes, 0)
			render_function.text = ""
			set_lod_controls_enabled.call(false)
		syncing["value"] = false

	list.item_selected.connect(func(index: int) -> void:
		sync.call(index)
	)
	threshold.value_changed.connect(func(value: float) -> void:
		if not syncing["value"] and data != null and selected["index"] >= 0:
			data.set_lod_field(selected["index"], "threshold", value)
	)
	attributes.value_changed.connect(func(value: float) -> void:
		if not syncing["value"] and data != null and selected["index"] >= 0:
			data.set_lod_field(selected["index"], "attributes", int(value))
	)
	render_function.text_submitted.connect(func(value: String) -> void:
		if not syncing["value"] and data != null and selected["index"] >= 0:
			data.set_lod_field(selected["index"], "render_function", value)
	)
	poly_lod.value_changed.connect(func(value: float) -> void:
		if not syncing["value"] and data != null:
			data.set_project_field("poly_collision_lod", int(value))
	)

	var pending_replace := {"value": false}
	add_button.pressed.connect(func() -> void:
		pending_replace["value"] = false
		scene_dialog.popup_centered(Vector2i(720, 480))
	)
	replace_button.pressed.connect(func() -> void:
		if selected["index"] < 0:
			return
		pending_replace["value"] = true
		scene_dialog.popup_centered(Vector2i(720, 480))
	)
	scene_dialog.file_selected.connect(func(path: String) -> void:
		var lod_index: int = int(selected["index"]) if bool(pending_replace["value"]) else -1
		if add_lod_scene(path, lod_index) == OK:
			var old_children := host.get_children()
			for child in old_children:
				host.remove_child(child)
				child.queue_free()
			_build_lods_inspector(host)
	)

	if not lods.is_empty():
		list.select(0)
		sync.call(0)
	else:
		sync.call(-1)


func _build_materials_inspector(host: Control, detail_only: bool = false) -> void:
	var box: VBoxContainer
	if detail_only:
		box = host as VBoxContainer
	else:
		box = _make_inspector_box(host)
	if box == null:
		return
	var materials := object_editor.object_data.get_materials() if object_editor and object_editor.object_data else []
	var shader_catalog := _shader_catalog()

	var detail_box := VBoxContainer.new()
	detail_box.name = "MaterialDetailPanel"
	detail_box.add_theme_constant_override("separation", 8)
	detail_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	if not detail_only:
		_part_anim_list = null
		_lights_list = null
		var list_panel := VBoxContainer.new()
		list_panel.name = "MaterialListPanel"
		list_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		box.add_child(list_panel)

		var summary := Label.new()
		summary.name = "MaterialListSummary"
		summary.theme_type_variation = &"Muted"
		summary.text = "%d materials" % materials.size()
		list_panel.add_child(summary)

		var list := ItemList.new()
		list.name = "MaterialsList"
		list.custom_minimum_size = Vector2(0, 360)
		list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		list.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_refresh_materials_list(list, materials)
		list_panel.add_child(list)
		_materials_list = list

		var material_toolbar := HBoxContainer.new()
		material_toolbar.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		list_panel.add_child(material_toolbar)

		var copy_button := Button.new()
		copy_button.name = "MaterialCopyButton"
		copy_button.text = "Copy"
		material_toolbar.add_child(copy_button)

		var paste_button := Button.new()
		paste_button.name = "MaterialPasteButton"
		paste_button.text = "Paste"
		paste_button.disabled = _material_clipboard.is_empty()
		material_toolbar.add_child(paste_button)
		_material_paste_button = paste_button

		if materials.is_empty():
			_material_selected_index = -1
		else:
			_material_selected_index = clampi(_material_selected_index, 0, materials.size() - 1)
			list.select(_material_selected_index)

		list.item_selected.connect(func(index: int) -> void:
			_material_selected_index = index
			_rebuild_object_detail_dock()
		)
		copy_button.pressed.connect(func() -> void:
			var current_materials := object_editor.object_data.get_materials() if object_editor and object_editor.object_data else []
			if _material_selected_index >= 0 and _material_selected_index < current_materials.size():
				_material_clipboard = (current_materials[_material_selected_index] as Dictionary).duplicate(true)
				paste_button.disabled = false
		)
		paste_button.pressed.connect(func() -> void:
			if _material_selected_index < 0 or _material_clipboard.is_empty() or object_editor == null or object_editor.object_data == null:
				return
			_paste_material_settings(_material_selected_index, _material_clipboard)
			_refresh_materials_left_list()
			_rebuild_object_detail_dock()
		)
		return

	box.add_child(detail_box)
	if materials.is_empty():
		var empty := Label.new()
		empty.name = "MaterialDetailEmpty"
		empty.theme_type_variation = &"Muted"
		empty.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		empty.text = "Select a material."
		detail_box.add_child(empty)
		return
	_material_selected_index = clampi(_material_selected_index, 0, materials.size() - 1)

	var shader_label := Label.new()
	shader_label.text = "Shader"
	detail_box.add_child(shader_label)

	var shader_option = ShaderTagPickerScript.new()
	shader_option.name = "ShaderTagPicker"
	shader_option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	detail_box.add_child(shader_option)
	var shader_option_legacy := OptionButton.new()
	shader_option_legacy.name = "ShaderTagOption"
	shader_option_legacy.visible = false
	detail_box.add_child(shader_option_legacy)

	var shader_status := Label.new()
	shader_status.name = "MaterialShaderStatus"
	shader_status.clip_text = true
	shader_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	detail_box.add_child(shader_status)

	var slot_controls := {}
	for slot_def in TEXTURE_SLOTS:
		var slot := int(slot_def.get("slot", 0))
		var slot_box := VBoxContainer.new()
		slot_box.name = "TextureSlot%dBox" % slot
		slot_box.add_theme_constant_override("separation", 4)
		detail_box.add_child(slot_box)

		var label := Label.new()
		label.text = String(slot_def.get("label", "Texture"))
		slot_box.add_child(label)

		var row := HBoxContainer.new()
		row.name = "TextureSlot%dRow" % slot
		row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		slot_box.add_child(row)

		var texture_widget = TextureSlotWidgetScript.new()
		texture_widget.name = "TextureSlot%dWidget" % slot
		texture_widget.configure(slot)
		texture_widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(texture_widget)

		var options_row := HBoxContainer.new()
		options_row.name = "TextureSlot%dOptionsRow" % slot
		options_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		slot_box.add_child(options_row)

		var clamped := CheckBox.new()
		clamped.name = "TextureSlot%dClamped" % slot
		clamped.text = "Clamp"
		options_row.add_child(clamped)

		var animated := CheckBox.new()
		animated.name = "TextureSlot%dAnimated" % slot
		animated.text = "Anim"
		options_row.add_child(animated)

		var frame := SpinBox.new()
		frame.name = "TextureSlot%dFrame" % slot
		frame.min_value = 0
		frame.max_value = 255
		frame.step = 1
		frame.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		options_row.add_child(frame)

		var status := Label.new()
		status.name = "TextureSlot%dStatus" % slot
		status.clip_text = true
		status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		slot_box.add_child(status)

		slot_controls[slot] = {
			"box": slot_box,
			"widget": texture_widget,
			"edit": texture_widget.line_edit,
			"status": status,
			"browse": texture_widget.browse_button,
			"clear": texture_widget.clear_button,
			"clamped": clamped,
			"animated": animated,
			"frame": frame,
		}

	var alpha_label := Label.new()
	alpha_label.text = "Alpha threshold"
	detail_box.add_child(alpha_label)

	var alpha := SpinBox.new()
	alpha.name = "AlphaThreshold"
	alpha.min_value = 0.0
	alpha.max_value = 1.0
	alpha.step = 0.01
	alpha.custom_minimum_size = Vector2(0, 34)
	detail_box.add_child(alpha)

	var generator_controls := _build_generator_controls(detail_box)

	var edit_frames := Button.new()
	edit_frames.name = "TextureAnimationFramesButton"
	edit_frames.text = "Edit frames..."
	detail_box.add_child(edit_frames)

	var anim_frames_dialog = AnimFramesDialogScript.new()
	anim_frames_dialog.name = "MaterialAnimFramesDialog"
	host.add_child(anim_frames_dialog)

	var texture_dialog := FileDialog.new()
	texture_dialog.name = "ObjectTextureFileDialog"
	texture_dialog.access = FileDialog.ACCESS_FILESYSTEM
	texture_dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	texture_dialog.filters = PackedStringArray([
		"*.tga,*.TGA,*.dds,*.DDS,*.mdt,*.MDT,*.pcx,*.PCX,*.png,*.PNG ; Object textures",
		"*.jpg,*.JPG,*.jpeg,*.JPEG,*.bmp,*.BMP ; Image fallbacks",
	])
	texture_dialog.min_size = Vector2i(760, 520)
	host.add_child(texture_dialog)

	var selected := {"index": _material_selected_index}
	var syncing := {"value": false}
	var pending_slot := {"slot": -1}

	var refresh_materials := func() -> void:
		materials = object_editor.object_data.get_materials() if object_editor and object_editor.object_data else []
		_refresh_materials_left_list()

	var sync_fields := func(index: int) -> void:
		syncing["value"] = true
		selected["index"] = index
		if index < 0 or index >= materials.size():
			syncing["value"] = false
			return
		var material: Dictionary = materials[index]
		shader_option.setup(shader_catalog, String(material.get("shader", "")))
		_populate_shader_tags(shader_option_legacy, String(material.get("shader", "")), shader_catalog)
		var shader_info := _shader_info_for_material(material, shader_catalog)
		shader_status.text = _shader_status_text(shader_info)
		alpha.value = float(material.get("alpha_threshold", 0.0))
		alpha.editable = bool(shader_info.get("is_alpha", false)) or (int(material.get("flags", 0)) & 0x01) != 0
		for slot_def in TEXTURE_SLOTS:
			var slot := int(slot_def.get("slot", 0))
			var controls: Dictionary = slot_controls.get(slot, {})
			var texture_info := _texture_for_slot(material, slot)
			var supported := _shader_supports_texture_slot(shader_info, slot)
			var occupied := not String(texture_info.get("name", "")).is_empty()
			var slot_box := controls.get("box") as Control
			var texture_widget = controls.get("widget")
			var edit := controls.get("edit") as LineEdit
			var status := controls.get("status") as Label
			var browse := controls.get("browse") as Button
			var clear := controls.get("clear") as Button
			var clamped := controls.get("clamped") as CheckBox
			var animated := controls.get("animated") as CheckBox
			var frame := controls.get("frame") as SpinBox
			if slot_box != null:
				slot_box.visible = supported or occupied
			if texture_widget != null:
				texture_widget.set_value(String(texture_info.get("name", "")))
				texture_widget.set_slot_enabled(supported)
				texture_widget.set_placeholder("Filename in object folder" if supported else "Unsupported by shader")
			if edit != null:
				edit.text = String(texture_info.get("name", ""))
				edit.editable = supported
				edit.placeholder_text = "Filename in object folder" if supported else "Unsupported by shader"
			if browse != null:
				browse.disabled = not supported
			if clear != null:
				clear.disabled = not supported or not occupied
			var texture_flags := int(texture_info.get("flags", 0))
			if clamped != null:
				clamped.button_pressed = (texture_flags & 0x02) != 0
				clamped.disabled = not supported or not occupied
			if animated != null:
				animated.button_pressed = (texture_flags & 0x01) != 0
				animated.disabled = not supported or not occupied
			if frame != null:
				frame.value = int(texture_info.get("frame", 0))
				frame.editable = supported and occupied
			if status != null:
				status.text = _describe_texture_status(index, texture_info, supported)
		_sync_generator_controls(generator_controls, material, shader_info)
		syncing["value"] = false

	var resync_selected := func() -> void:
		var current_index := int(selected.get("index", -1))
		if current_index >= materials.size():
			current_index = materials.size() - 1
		_material_selected_index = current_index
		selected["index"] = current_index
		refresh_materials.call()
		if current_index >= 0 and current_index < materials.size():
			sync_fields.call(current_index)

	var apply_slot_name := func(slot: int, value: String) -> void:
		var current_index := int(selected.get("index", -1))
		if current_index < 0 or object_editor == null or object_editor.object_data == null:
			return
		object_editor.object_data.set_material_texture_slot(current_index, slot, value.get_file())
		resync_selected.call()

	var apply_slot_options := func(slot: int) -> void:
		var current_index := int(selected.get("index", -1))
		if current_index < 0 or object_editor == null or object_editor.object_data == null:
			return
		var controls: Dictionary = slot_controls.get(slot, {})
		var clamped := controls.get("clamped") as CheckBox
		var animated := controls.get("animated") as CheckBox
		var frame := controls.get("frame") as SpinBox
		var flags := 0
		if animated != null and animated.button_pressed:
			flags |= 0x01
		if clamped != null and clamped.button_pressed:
			flags |= 0x02
		var type := 4 if slot == 3 or slot == 4 else 0
		object_editor.object_data.set_material_texture_slot_options(current_index, slot, flags, int(frame.value) if frame != null else 0, type)
		resync_selected.call()

	var apply_uv_generator := func(axis: String, params: Dictionary) -> void:
		if syncing["value"] or selected["index"] < 0:
			return
		object_editor.object_data.set_material_uv_generator(selected["index"], axis, params)
		resync_selected.call()

	var apply_rgb_generator := func(params: Dictionary) -> void:
		if syncing["value"] or selected["index"] < 0:
			return
		object_editor.object_data.set_material_rgb_generator(selected["index"], params)
		resync_selected.call()

	var apply_alpha_generator := func(params: Dictionary) -> void:
		if syncing["value"] or selected["index"] < 0:
			return
		object_editor.object_data.set_material_alpha_generator(selected["index"], params)
		resync_selected.call()

	var apply_texture_animation := func(params: Dictionary) -> void:
		if syncing["value"] or selected["index"] < 0:
			return
		object_editor.object_data.set_material_texture_animation(selected["index"], params)
		resync_selected.call()

	var show_slot_message := func(slot: int, message: String) -> void:
		var controls: Dictionary = slot_controls.get(slot, {})
		var status := controls.get("status") as Label
		if status != null:
			status.text = message

	shader_option.value_changed.connect(func(tag: String) -> void:
		if syncing["value"] or selected["index"] < 0:
			return
		object_editor.object_data.set_material_shader(selected["index"], tag)
		resync_selected.call()
	)

	for slot_def in TEXTURE_SLOTS:
		var slot := int(slot_def.get("slot", 0))
		var controls: Dictionary = slot_controls.get(slot, {})
		var texture_widget = controls.get("widget")
		var edit := controls.get("edit") as LineEdit
		if texture_widget != null:
			texture_widget.value_changed.connect(func(slot_id: int, value: String) -> void:
				apply_slot_name.call(slot_id, value)
			)
			texture_widget.browse_requested.connect(func(slot_id: int) -> void:
				pending_slot["slot"] = slot_id
				var source_dir := object_editor.object_data.get_source_dir() if object_editor and object_editor.object_data else ""
				if source_dir.is_empty():
					show_slot_message.call(slot_id, "Open an object before choosing a texture.")
					return
				texture_dialog.current_dir = source_dir
				texture_dialog.popup_centered()
			)
		if edit != null:
			edit.text_submitted.connect(func(value: String, slot_id: int = slot) -> void:
				apply_slot_name.call(slot_id, value)
			)
		var clamped := controls.get("clamped") as CheckBox
		var animated := controls.get("animated") as CheckBox
		var frame := controls.get("frame") as SpinBox
		if clamped != null:
			clamped.toggled.connect(func(_pressed: bool, slot_id: int = slot) -> void:
				if not syncing["value"]:
					apply_slot_options.call(slot_id)
			)
		if animated != null:
			animated.toggled.connect(func(_pressed: bool, slot_id: int = slot) -> void:
				if not syncing["value"]:
					apply_slot_options.call(slot_id)
			)
		if frame != null:
			frame.value_changed.connect(func(_value: float, slot_id: int = slot) -> void:
				if not syncing["value"]:
					apply_slot_options.call(slot_id)
			)

	texture_dialog.file_selected.connect(func(path: String) -> void:
		var slot := int(pending_slot.get("slot", -1))
		if slot < 0:
			return
		var file_name := _object_folder_filename(path)
		if file_name.is_empty():
			show_slot_message.call(slot, "Choose a file in the object folder.")
			return
		apply_slot_name.call(slot, file_name)
	)
	alpha.value_changed.connect(func(value: float) -> void:
		if not syncing["value"] and selected["index"] >= 0:
			object_editor.object_data.set_material_alpha_threshold(selected["index"], value)
			resync_selected.call()
	)
	for axis in ["u", "v"]:
		var axis_id := String(axis)
		var axis_controls: Dictionary = generator_controls.get(axis_id, {})
		for key in ["style", "phase", "reg", "rate", "start", "end"]:
			var spin := axis_controls.get(key) as SpinBox
			if spin != null:
				_connect_uv_generator_spin(spin, axis_id, String(key), generator_controls, apply_uv_generator)
		var style_option := axis_controls.get("style_option") as OptionButton
		if style_option != null:
			style_option.item_selected.connect(func(index: int, selected_axis: String = axis_id, option: OptionButton = style_option) -> void:
				var params := _uv_generator_params(generator_controls, selected_axis)
				params["style"] = option.get_item_id(index)
				apply_uv_generator.call(selected_axis, params)
			)
		var reg_picker = axis_controls.get("reg_picker")
		if reg_picker != null:
			reg_picker.register_selected.connect(func(reg: int, selected_axis: String = axis_id) -> void:
				var params := _uv_generator_params(generator_controls, selected_axis)
				params["reg"] = reg
				apply_uv_generator.call(selected_axis, params)
			)
	for key in ["style", "phase", "reg", "rate"]:
		var spin := generator_controls.get("rgb_%s" % key) as SpinBox
		if spin != null:
			_connect_rgb_generator_spin(spin, String(key), generator_controls, apply_rgb_generator)
	var rgb_style_option := generator_controls.get("rgb_style_option") as OptionButton
	if rgb_style_option != null:
		rgb_style_option.item_selected.connect(func(index: int) -> void:
			var params := _rgb_generator_params(generator_controls)
			params["style"] = rgb_style_option.get_item_id(index)
			apply_rgb_generator.call(params)
		)
	var rgb_reg_picker = generator_controls.get("rgb_reg_picker")
	if rgb_reg_picker != null:
		rgb_reg_picker.register_selected.connect(func(reg: int) -> void:
			var params := _rgb_generator_params(generator_controls)
			params["reg"] = reg
			apply_rgb_generator.call(params)
		)
	var rgb_start := generator_controls.get("rgb_start_color") as ColorPickerButton
	var rgb_end := generator_controls.get("rgb_end_color") as ColorPickerButton
	if rgb_start != null:
		rgb_start.color_changed.connect(func(_color: Color) -> void:
			apply_rgb_generator.call(_rgb_generator_params(generator_controls))
		)
	if rgb_end != null:
		rgb_end.color_changed.connect(func(_color: Color) -> void:
			apply_rgb_generator.call(_rgb_generator_params(generator_controls))
		)
	for key in ["style", "phase", "reg", "rate", "start", "end"]:
		var spin := generator_controls.get("alpha_%s" % key) as SpinBox
		if spin != null:
			_connect_alpha_generator_spin(spin, String(key), generator_controls, apply_alpha_generator)
	var alpha_style_option := generator_controls.get("alpha_style_option") as OptionButton
	if alpha_style_option != null:
		alpha_style_option.item_selected.connect(func(index: int) -> void:
			var params := _alpha_generator_params(generator_controls)
			params["style"] = alpha_style_option.get_item_id(index)
			apply_alpha_generator.call(params)
		)
	var alpha_reg_picker = generator_controls.get("alpha_reg_picker")
	if alpha_reg_picker != null:
		alpha_reg_picker.register_selected.connect(func(reg: int) -> void:
			var params := _alpha_generator_params(generator_controls)
			params["reg"] = reg
			apply_alpha_generator.call(params)
		)
	for key in ["frames", "type", "time"]:
		var spin := generator_controls.get("anim_%s" % key) as SpinBox
		if spin != null:
			_connect_texture_animation_spin(spin, String(key), generator_controls, apply_texture_animation)
	edit_frames.pressed.connect(func() -> void:
		var current_index := int(selected.get("index", -1))
		if current_index < 0 or current_index >= materials.size():
			return
		var material: Dictionary = materials[current_index]
		var shader_info := _shader_info_for_material(material, shader_catalog)
		anim_frames_dialog.setup(object_editor.object_data, current_index, shader_info)
		anim_frames_dialog.popup_centered()
	)
	anim_frames_dialog.frame_changed.connect(func() -> void:
		resync_selected.call()
	)
	if not materials.is_empty():
		sync_fields.call(_material_selected_index)


func _paste_material_settings(target_index: int, material: Dictionary) -> void:
	if object_editor == null or object_editor.object_data == null:
		return
	object_editor.object_data.set_material_shader(target_index, String(material.get("shader", "")))
	object_editor.object_data.set_material_alpha_threshold(target_index, float(material.get("alpha_threshold", 0.0)))
	for slot_def in TEXTURE_SLOTS:
		var slot := int(slot_def.get("slot", 0))
		var texture := _texture_for_slot(material, slot)
		object_editor.object_data.set_material_texture_slot(target_index, slot, String(texture.get("name", "")))
		if not String(texture.get("name", "")).is_empty():
			object_editor.object_data.set_material_texture_slot_options(
					target_index,
					slot,
					int(texture.get("flags", 0)),
					int(texture.get("frame", 0)),
					int(texture.get("type", 0)))
	for axis in ["u", "v"]:
		var key := "%s_params" % axis
		if material.has(key):
			object_editor.object_data.set_material_uv_generator(target_index, axis, material.get(key, {}))
	if material.has("rgb_gen"):
		object_editor.object_data.set_material_rgb_generator(target_index, material.get("rgb_gen", {}))
	if material.has("alpha_gen"):
		object_editor.object_data.set_material_alpha_generator(target_index, material.get("alpha_gen", {}))
	if material.has("animation"):
		object_editor.object_data.set_material_texture_animation(target_index, material.get("animation", {}))


func _refresh_materials_list(list: ItemList, materials: Array) -> void:
	var selected_items := list.get_selected_items()
	var selected_index := int(selected_items[0]) if selected_items.size() > 0 else -1
	list.clear()
	for material in materials:
		list.add_item("%02d  %s" % [int(material.get("index", 0)), String(material.get("shader", ""))])
	if selected_index >= 0 and selected_index < materials.size():
		list.select(selected_index)


func _refresh_materials_left_list() -> void:
	if _materials_list == null or not is_instance_valid(_materials_list):
		return
	var materials := object_editor.object_data.get_materials() if object_editor and object_editor.object_data else []
	if materials.is_empty():
		_material_selected_index = -1
	else:
		_material_selected_index = clampi(_material_selected_index, 0, materials.size() - 1)
	_refresh_materials_list(_materials_list, materials)
	if _material_selected_index >= 0 and _material_selected_index < materials.size():
		_materials_list.select(_material_selected_index)
	if _material_paste_button != null and is_instance_valid(_material_paste_button):
		_material_paste_button.disabled = _material_clipboard.is_empty()


func _populate_shader_tags(option: OptionButton, current: String, shader_catalog: Array) -> void:
	option.clear()
	var known_tags := []
	for shader in shader_catalog:
		known_tags.append(String(shader.get("name", "")))
	if not current.is_empty() and not known_tags.has(current):
		option.add_item(current)
	for shader in shader_catalog:
		var shader_tag := String(shader.get("name", ""))
		if shader_tag.is_empty():
			continue
		option.add_item(shader_tag)
	var match_index := _shader_index(option, current)
	if match_index >= 0:
		option.select(match_index)
	elif option.get_item_count() > 0:
		option.select(0)


func _shader_index(option: OptionButton, tag: String) -> int:
	for i in range(option.get_item_count()):
		if option.get_item_text(i) == tag:
			return i
	return -1


func _texture_for_slot(material: Dictionary, slot: int) -> Dictionary:
	var textures: Array = material.get("textures", [])
	for i in range(textures.size()):
		var texture: Dictionary = textures[i]
		if int(texture.get("slot", 0)) == slot:
			return {
				"texture_index": i,
				"name": String(texture.get("name", "")),
				"flags": int(texture.get("flags", 0)),
				"frame": int(texture.get("frame", 0)),
				"type": int(texture.get("type", 0)),
				"resolved_path": String(texture.get("resolved_path", "")),
			}
	return {
		"texture_index": -1,
		"name": "",
		"flags": 0,
		"frame": 0,
		"type": 0,
		"resolved_path": "",
	}


func _describe_texture_status(material_index: int, texture_info: Dictionary, supported: bool = true) -> String:
	var name := String(texture_info.get("name", ""))
	if name.is_empty():
		return "Empty" if supported else "Unsupported by shader"
	var texture_index := int(texture_info.get("texture_index", -1))
	if object_editor == null or object_editor.object_data == null or texture_index < 0:
		return "Present but unsupported by shader" if not supported else "Missing in object folder"
	var resolved := String(texture_info.get("resolved_path", ""))
	if resolved.is_empty():
		resolved = object_editor.object_data.resolve_material_texture_path(material_index, texture_index)
	if resolved.is_empty():
		return "Present but unsupported by shader; missing in object folder" if not supported else "Missing in object folder"
	var prefix := "Unsupported by shader; resolved" if not supported else "Resolved"
	return "%s: %s" % [prefix, resolved.get_file()]


func _shader_catalog() -> Array:
	if object_editor != null and object_editor.object_data != null and object_editor.object_data.has_method("get_shader_catalog"):
		var catalog: Array = object_editor.object_data.get_shader_catalog()
		if not catalog.is_empty():
			return catalog
	return []


func _shader_info_for_material(material: Dictionary, shader_catalog: Array) -> Dictionary:
	var shader_name := String(material.get("shader", ""))
	for shader in shader_catalog:
		if String(shader.get("name", "")) == shader_name:
			return shader
	var info := {}
	for key in ["shader_flags", "has_diffuse", "has_secondary", "has_normal_a", "has_normal_b", "is_alpha", "is_luminance", "is_glass_shader", "is_skinned_shader", "is_special_shader", "uses_uv_generators", "uses_environment", "uses_specular", "uses_flag_animation", "shader_family", "shader_blend", "normal_space"]:
		if material.has(key):
			info[key] = material[key]
	info["name"] = shader_name
	return info


func _shader_supports_texture_slot(shader_info: Dictionary, slot: int) -> bool:
	match slot:
		1:
			return bool(shader_info.get("has_diffuse", false))
		2:
			return bool(shader_info.get("has_secondary", false))
		3:
			return bool(shader_info.get("has_normal_a", false))
		4:
			return bool(shader_info.get("has_normal_b", false))
		_:
			return false


func _shader_status_text(shader_info: Dictionary) -> String:
	var bits := []
	if bool(shader_info.get("has_diffuse", false)):
		bits.append("diffuse")
	if bool(shader_info.get("has_secondary", false)):
		bits.append("detail")
	if bool(shader_info.get("has_normal_a", false)):
		bits.append("normal")
	if bool(shader_info.get("has_normal_b", false)):
		bits.append("normal B")
	if bool(shader_info.get("is_alpha", false)):
		bits.append("alpha")
	if bool(shader_info.get("is_luminance", false)):
		bits.append("luminance")
	if bool(shader_info.get("is_glass_shader", false)):
		bits.append("glass")
	if bool(shader_info.get("uses_uv_generators", false)):
		bits.append("UV gen")
	return "Enabled: %s" % ", ".join(bits) if not bits.is_empty() else "No material options enabled by this shader."


func _build_generator_controls(box: VBoxContainer) -> Dictionary:
	var controls := {}
	var title := Label.new()
	title.text = "Generators"
	box.add_child(title)

	for axis in ["u", "v"]:
		var section := VBoxContainer.new()
		section.name = "%sGeneratorSection" % axis.to_upper()
		section.add_theme_constant_override("separation", 4)
		box.add_child(section)
		var header := Label.new()
		header.text = "%s map function" % axis.to_upper()
		section.add_child(header)
		var axis_controls := {
			"box": section,
			"style_option": _add_id_option_row(section, "%sGeneratorStyleOption" % axis.to_upper(), "Style", GENERATOR_STYLE_OPTIONS),
			"style": _add_spin_row(section, "%sGeneratorStyle" % axis.to_upper(), "Style", 0, 255, 1),
			"phase": _add_spin_row(section, "%sGeneratorPhase" % axis.to_upper(), "Phase", -100000, 100000, 0.01),
			"reg_picker": _add_ctrl_reg_row(section, "%sGeneratorControlReg" % axis.to_upper(), "Control reg"),
			"reg": _add_spin_row(section, "%sGeneratorReg" % axis.to_upper(), "Control reg", -1, 255, 1),
			"rate": _add_spin_row(section, "%sGeneratorRate" % axis.to_upper(), "Rate", -100000, 100000, 0.01),
			"start": _add_spin_row(section, "%sGeneratorStart" % axis.to_upper(), "Start", -100000, 100000, 0.01),
			"end": _add_spin_row(section, "%sGeneratorEnd" % axis.to_upper(), "End", -100000, 100000, 0.01),
		}
		controls[axis] = axis_controls

	var rgb_section := VBoxContainer.new()
	rgb_section.name = "RgbGeneratorSection"
	rgb_section.add_theme_constant_override("separation", 4)
	box.add_child(rgb_section)
	var rgb_header := Label.new()
	rgb_header.text = "RGB gen"
	rgb_section.add_child(rgb_header)
	controls["rgb_box"] = rgb_section
	controls["rgb_style_option"] = _add_id_option_row(rgb_section, "RgbGeneratorStyleOption", "Style", GENERATOR_STYLE_OPTIONS)
	controls["rgb_style"] = _add_spin_row(rgb_section, "RgbGeneratorStyle", "Style", 0, 255, 1)
	controls["rgb_phase"] = _add_spin_row(rgb_section, "RgbGeneratorPhase", "Phase", -100000, 100000, 0.01)
	controls["rgb_reg_picker"] = _add_ctrl_reg_row(rgb_section, "RgbGeneratorControlReg", "Control reg")
	controls["rgb_reg"] = _add_spin_row(rgb_section, "RgbGeneratorReg", "Control reg", -1, 255, 1)
	controls["rgb_rate"] = _add_spin_row(rgb_section, "RgbGeneratorRate", "Rate", -100000, 100000, 0.01)
	controls["rgb_start_color"] = _add_color_row(rgb_section, "RgbGeneratorStartColor", "Start")
	controls["rgb_end_color"] = _add_color_row(rgb_section, "RgbGeneratorEndColor", "End")

	var alpha_section := VBoxContainer.new()
	alpha_section.name = "AlphaGeneratorSection"
	alpha_section.add_theme_constant_override("separation", 4)
	box.add_child(alpha_section)
	var alpha_header := Label.new()
	alpha_header.text = "Alpha gen"
	alpha_section.add_child(alpha_header)
	controls["alpha_box"] = alpha_section
	controls["alpha_style_option"] = _add_id_option_row(alpha_section, "AlphaGeneratorStyleOption", "Style", GENERATOR_STYLE_OPTIONS)
	controls["alpha_style"] = _add_spin_row(alpha_section, "AlphaGeneratorStyle", "Style", 0, 255, 1)
	controls["alpha_phase"] = _add_spin_row(alpha_section, "AlphaGeneratorPhase", "Phase", -100000, 100000, 0.01)
	controls["alpha_reg_picker"] = _add_ctrl_reg_row(alpha_section, "AlphaGeneratorControlReg", "Control reg")
	controls["alpha_reg"] = _add_spin_row(alpha_section, "AlphaGeneratorReg", "Control reg", -1, 255, 1)
	controls["alpha_rate"] = _add_spin_row(alpha_section, "AlphaGeneratorRate", "Rate", -100000, 100000, 0.01)
	controls["alpha_start"] = _add_spin_row(alpha_section, "AlphaGeneratorStart", "Start", -32768, 32767, 1)
	controls["alpha_end"] = _add_spin_row(alpha_section, "AlphaGeneratorEnd", "End", -32768, 32767, 1)

	var anim_section := VBoxContainer.new()
	anim_section.name = "TextureAnimationSection"
	anim_section.add_theme_constant_override("separation", 4)
	box.add_child(anim_section)
	var anim_header := Label.new()
	anim_header.text = "Texture animation"
	anim_section.add_child(anim_header)
	controls["anim_box"] = anim_section
	controls["anim_frames"] = _add_spin_row(anim_section, "TextureAnimationFrames", "Frames", 0, 255, 1)
	controls["anim_type"] = _add_spin_row(anim_section, "TextureAnimationType", "Type", 0, 255, 1)
	controls["anim_time"] = _add_spin_row(anim_section, "TextureAnimationTime", "Frame time / reg", -32768, 32767, 1)
	return controls


func _add_spin_row(parent: Control, node_name: String, label_text: String, min_value: float, max_value: float, step: float) -> SpinBox:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.tooltip_text = label_text
	label.clip_text = true
	label.custom_minimum_size = Vector2(76, 0)
	row.add_child(label)
	var spin := SpinBox.new()
	spin.name = node_name
	spin.min_value = min_value
	spin.max_value = max_value
	spin.step = step
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(spin)
	return spin


func _add_color_row(parent: Control, node_name: String, label_text: String) -> ColorPickerButton:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.tooltip_text = label_text
	label.clip_text = true
	label.custom_minimum_size = Vector2(76, 0)
	row.add_child(label)
	var picker := ColorPickerButton.new()
	picker.name = node_name
	picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(picker)
	return picker


func _add_id_option_row(parent: Control, node_name: String, label_text: String, options: Array) -> OptionButton:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.tooltip_text = label_text
	label.clip_text = true
	label.custom_minimum_size = Vector2(76, 0)
	row.add_child(label)
	var option := OptionButton.new()
	option.name = node_name
	option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(option)
	_populate_id_option(option, options, 0)
	return option


func _add_ctrl_reg_row(parent: Control, node_name: String, label_text: String):
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.tooltip_text = label_text
	label.clip_text = true
	label.custom_minimum_size = Vector2(76, 0)
	row.add_child(label)
	var picker = CtrlRegPickerScript.new()
	picker.name = node_name
	picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(picker)
	picker.setup(_control_registers(), -1)
	return picker


func _add_detail_field(parent: Control, label_text: String) -> VBoxContainer:
	var row := VBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_theme_constant_override("separation", 3)
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.tooltip_text = label_text
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(label)
	return row


func _add_detail_spin_row(parent: Control, node_name: String, label_text: String, min_value: float, max_value: float, step: float) -> SpinBox:
	var row := _add_detail_field(parent, label_text)
	var spin := SpinBox.new()
	spin.name = node_name
	spin.min_value = min_value
	spin.max_value = max_value
	spin.step = step
	spin.custom_minimum_size = Vector2(0, 32)
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(spin)
	return spin


func _add_detail_id_option_row(parent: Control, node_name: String, label_text: String, options: Array) -> OptionButton:
	var row := _add_detail_field(parent, label_text)
	var option := OptionButton.new()
	option.name = node_name
	option.fit_to_longest_item = false
	option.custom_minimum_size = Vector2(0, 32)
	option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(option)
	_populate_id_option(option, options, 0)
	return option


func _add_detail_ctrl_reg_row(parent: Control, node_name: String, label_text: String):
	var row := _add_detail_field(parent, label_text)
	var picker = CtrlRegPickerScript.new()
	picker.name = node_name
	picker.fit_to_longest_item = false
	picker.custom_minimum_size = Vector2(0, 32)
	picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(picker)
	picker.setup(_control_registers(), -1)
	return picker


func _populate_id_option(option: OptionButton, options: Array, current_id: int) -> void:
	if option == null:
		return
	option.clear()
	var selected_index := 0
	var matched := false
	for item in options:
		var item_id := int(item.get("id", 0))
		option.add_item(String(item.get("label", str(item_id))), item_id)
		var option_index := option.get_item_count() - 1
		if item_id == current_id:
			selected_index = option_index
			matched = true
	if not matched:
		option.add_item("Custom %d" % current_id, current_id)
		selected_index = option.get_item_count() - 1
	option.select(selected_index)


func _selected_option_id(option: OptionButton) -> int:
	if option == null or option.selected < 0 or option.selected >= option.get_item_count():
		return 0
	return option.get_item_id(option.selected)


func _control_registers() -> Array:
	if object_editor != null and object_editor.object_data != null and object_editor.object_data.has_method("get_control_registers"):
		return object_editor.object_data.get_control_registers()
	return []


func _sync_generator_controls(controls: Dictionary, material: Dictionary, shader_info: Dictionary) -> void:
	var u_params: Dictionary = material.get("u_params", {})
	var v_params: Dictionary = material.get("v_params", {})
	_sync_uv_axis_controls(controls.get("u", {}), u_params)
	_sync_uv_axis_controls(controls.get("v", {}), v_params)

	var uses_uv := bool(shader_info.get("uses_uv_generators", false))
	var has_uv_data := int(u_params.get("style", 0)) > 0 or int(v_params.get("style", 0)) > 0
	_set_section_visible_enabled(controls.get("u", {}).get("box"), uses_uv or has_uv_data, uses_uv)
	_set_section_visible_enabled(controls.get("v", {}).get("box"), uses_uv or has_uv_data, uses_uv)

	var rgb_gen: Dictionary = material.get("rgb_gen", {})
	_populate_id_option(controls.get("rgb_style_option") as OptionButton, GENERATOR_STYLE_OPTIONS, int(rgb_gen.get("style", 0)))
	_set_spin(controls.get("rgb_style"), int(rgb_gen.get("style", 0)))
	_set_spin(controls.get("rgb_phase"), float(rgb_gen.get("phase", 0.0)))
	var rgb_reg_picker = controls.get("rgb_reg_picker")
	if rgb_reg_picker != null:
		rgb_reg_picker.setup(_control_registers(), int(rgb_gen.get("reg", -1)))
	_set_spin(controls.get("rgb_reg"), int(rgb_gen.get("reg", -1)))
	_set_spin(controls.get("rgb_rate"), float(rgb_gen.get("rate", 0.0)))
	var rgb_start := controls.get("rgb_start_color") as ColorPickerButton
	var rgb_end := controls.get("rgb_end_color") as ColorPickerButton
	if rgb_start != null:
		rgb_start.color = rgb_gen.get("start_color", Color.WHITE)
	if rgb_end != null:
		rgb_end.color = rgb_gen.get("end_color", Color.WHITE)
	var rgb_enabled := bool(shader_info.get("is_luminance", false))
	var rgb_visible := rgb_enabled or int(rgb_gen.get("style", 0)) > 0
	_set_section_visible_enabled(controls.get("rgb_box"), rgb_visible, rgb_enabled)

	var alpha_gen: Dictionary = material.get("alpha_gen", {})
	_populate_id_option(controls.get("alpha_style_option") as OptionButton, GENERATOR_STYLE_OPTIONS, int(alpha_gen.get("style", 0)))
	_set_spin(controls.get("alpha_style"), int(alpha_gen.get("style", 0)))
	_set_spin(controls.get("alpha_phase"), float(alpha_gen.get("phase", 0.0)))
	var alpha_reg_picker = controls.get("alpha_reg_picker")
	if alpha_reg_picker != null:
		alpha_reg_picker.setup(_control_registers(), int(alpha_gen.get("reg", -1)))
	_set_spin(controls.get("alpha_reg"), int(alpha_gen.get("reg", -1)))
	_set_spin(controls.get("alpha_rate"), float(alpha_gen.get("rate", 0.0)))
	_set_spin(controls.get("alpha_start"), int(alpha_gen.get("start", 0)))
	_set_spin(controls.get("alpha_end"), int(alpha_gen.get("end", 0)))
	var alpha_enabled := bool(shader_info.get("is_alpha", false))
	var alpha_visible := alpha_enabled or int(alpha_gen.get("style", 0)) > 0
	_set_section_visible_enabled(controls.get("alpha_box"), alpha_visible, alpha_enabled)

	var animation: Dictionary = material.get("animation", {})
	_set_spin(controls.get("anim_frames"), int(animation.get("num_frames", 0)))
	_set_spin(controls.get("anim_type"), int(animation.get("animation_type", 0)))
	_set_spin(controls.get("anim_time"), int(animation.get("cycle_frame_time", 0)))
	var anim_enabled := bool(shader_info.get("has_diffuse", false))
	var anim_visible := anim_enabled or int(animation.get("num_frames", 0)) > 0
	_set_section_visible_enabled(controls.get("anim_box"), anim_visible, anim_enabled)


func _sync_uv_axis_controls(axis_controls: Dictionary, params: Dictionary) -> void:
	_populate_id_option(axis_controls.get("style_option") as OptionButton, GENERATOR_STYLE_OPTIONS, int(params.get("style", 0)))
	_set_spin(axis_controls.get("style"), int(params.get("style", 0)))
	_set_spin(axis_controls.get("phase"), float(params.get("phase", 0.0)))
	var reg_picker = axis_controls.get("reg_picker")
	if reg_picker != null:
		reg_picker.setup(_control_registers(), int(params.get("reg", -1)))
	_set_spin(axis_controls.get("reg"), int(params.get("reg", -1)))
	_set_spin(axis_controls.get("rate"), float(params.get("rate", 0.0)))
	_set_spin(axis_controls.get("start"), float(params.get("start", 0.0)))
	_set_spin(axis_controls.get("end"), float(params.get("end", 0.0)))


func _set_spin(node, value: float) -> void:
	var spin := node as SpinBox
	if spin != null:
		spin.value = value


func _set_section_visible_enabled(node, visible: bool, enabled: bool) -> void:
	var control := node as Control
	if control == null:
		return
	control.visible = visible
	_set_controls_enabled(control, enabled)


func _set_controls_enabled(node: Node, enabled: bool) -> void:
	if node is SpinBox:
		(node as SpinBox).editable = enabled
	elif node is LineEdit:
		(node as LineEdit).editable = enabled
	elif node is Button:
		(node as Button).disabled = not enabled
	for child in node.get_children():
		_set_controls_enabled(child, enabled)


func _connect_uv_generator_spin(spin: SpinBox, axis: String, key: String, controls: Dictionary, apply: Callable) -> void:
	spin.value_changed.connect(func(value: float) -> void:
		var params := _uv_generator_params(controls, axis)
		if key == "style" or key == "reg":
			params[key] = int(value)
		else:
			params[key] = value
		apply.call(axis, params)
	)


func _connect_rgb_generator_spin(spin: SpinBox, key: String, controls: Dictionary, apply: Callable) -> void:
	spin.value_changed.connect(func(value: float) -> void:
		var params := _rgb_generator_params(controls)
		if key == "style" or key == "reg":
			params[key] = int(value)
		else:
			params[key] = value
		apply.call(params)
	)


func _connect_alpha_generator_spin(spin: SpinBox, key: String, controls: Dictionary, apply: Callable) -> void:
	spin.value_changed.connect(func(value: float) -> void:
		var params := _alpha_generator_params(controls)
		if key == "phase" or key == "rate":
			params[key] = value
		else:
			params[key] = int(value)
		apply.call(params)
	)


func _connect_texture_animation_spin(spin: SpinBox, key: String, controls: Dictionary, apply: Callable) -> void:
	spin.value_changed.connect(func(value: float) -> void:
		var params := _texture_animation_params(controls)
		match key:
			"frames":
				params["num_frames"] = int(value)
			"type":
				params["animation_type"] = int(value)
			"time":
				params["cycle_frame_time"] = int(value)
		apply.call(params)
	)


func _uv_generator_params(controls: Dictionary, axis: String) -> Dictionary:
	var axis_controls: Dictionary = controls.get(axis, {})
	return {
		"style": int((axis_controls.get("style") as SpinBox).value),
		"phase": float((axis_controls.get("phase") as SpinBox).value),
		"reg": int((axis_controls.get("reg") as SpinBox).value),
		"rate": float((axis_controls.get("rate") as SpinBox).value),
		"start": float((axis_controls.get("start") as SpinBox).value),
		"end": float((axis_controls.get("end") as SpinBox).value),
	}


func _rgb_generator_params(controls: Dictionary) -> Dictionary:
	return {
		"style": int((controls.get("rgb_style") as SpinBox).value),
		"phase": float((controls.get("rgb_phase") as SpinBox).value),
		"reg": int((controls.get("rgb_reg") as SpinBox).value),
		"rate": float((controls.get("rgb_rate") as SpinBox).value),
		"start_color": (controls.get("rgb_start_color") as ColorPickerButton).color,
		"end_color": (controls.get("rgb_end_color") as ColorPickerButton).color,
	}


func _alpha_generator_params(controls: Dictionary) -> Dictionary:
	return {
		"style": int((controls.get("alpha_style") as SpinBox).value),
		"phase": float((controls.get("alpha_phase") as SpinBox).value),
		"reg": int((controls.get("alpha_reg") as SpinBox).value),
		"rate": float((controls.get("alpha_rate") as SpinBox).value),
		"start": int((controls.get("alpha_start") as SpinBox).value),
		"end": int((controls.get("alpha_end") as SpinBox).value),
	}


func _texture_animation_params(controls: Dictionary) -> Dictionary:
	return {
		"num_frames": int((controls.get("anim_frames") as SpinBox).value),
		"animation_type": int((controls.get("anim_type") as SpinBox).value),
		"cycle_frame_time": int((controls.get("anim_time") as SpinBox).value),
	}


func _object_folder_filename(path: String) -> String:
	if object_editor == null or object_editor.object_data == null:
		return ""
	var source_dir := object_editor.object_data.get_source_dir()
	if source_dir.is_empty() or path.is_empty():
		return ""
	var source := ProjectSettings.globalize_path(source_dir).replace("\\", "/").trim_suffix("/")
	var selected := ProjectSettings.globalize_path(path).replace("\\", "/")
	var selected_lower := selected.to_lower()
	var source_lower := source.to_lower()
	if not selected_lower.begins_with(source_lower + "/"):
		return ""
	return selected.get_file()


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


func _rebuild_object_detail_dock() -> void:
	if _object_detail_dock == null or not is_instance_valid(_object_detail_dock):
		return
	for child in _object_detail_dock.get_children():
		_object_detail_dock.remove_child(child)
		child.free()

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 10)
	margin.add_theme_constant_override("margin_top", 10)
	margin.add_theme_constant_override("margin_right", 10)
	margin.add_theme_constant_override("margin_bottom", 10)
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
			_build_materials_inspector(box, true)
		Workflow.LIGHTS:
			_build_lights_inspector(box, true)
		Workflow.LODS:
			_build_object_detail_placeholder(box, "LODs", "Select a LOD in the left pane to edit scene bindings and thresholds.")
		_:
			_build_object_detail_placeholder(box, "Object", "Preview controls are in the left pane. The center viewport stays reserved for the live object preview.")


func _build_object_detail_placeholder(box: VBoxContainer, title: String, message: String) -> void:
	var heading := Label.new()
	heading.theme_type_variation = &"Heading"
	heading.text = title
	box.add_child(heading)

	var body := Label.new()
	body.theme_type_variation = &"Muted"
	body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	body.text = message
	box.add_child(body)


func _build_channel_card(parent: VBoxContainer, node_name: String) -> VBoxContainer:
	var card := PanelContainer.new()
	card.name = node_name
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(card)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 8)
	margin.add_theme_constant_override("margin_top", 8)
	margin.add_theme_constant_override("margin_right", 8)
	margin.add_theme_constant_override("margin_bottom", 8)
	card.add_child(margin)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.add_child(box)
	return box


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
			translation_register.get_parent().visible = translation_mode.get_selected_id() == 113 or translation_mode.get_selected_id() == 114
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


func _refresh_lights_list(list: ItemList, lights: Array) -> void:
	list.clear()
	for light in lights:
		list.add_item("%02d  part %d" % [int(light.get("index", 0)), int(light.get("part_index", 0))])


func _refresh_lights_left_list() -> void:
	if _lights_list == null or not is_instance_valid(_lights_list):
		return
	var lights := object_editor.object_data.get_lights() if object_editor and object_editor.object_data else []
	if lights.is_empty():
		_light_selected_index = -1
	else:
		_light_selected_index = clampi(_light_selected_index, 0, lights.size() - 1)
	_refresh_lights_list(_lights_list, lights)
	if _light_selected_index >= 0 and _light_selected_index < lights.size():
		_lights_list.select(_light_selected_index)


func _build_lights_inspector(host: Control, detail_only: bool = false) -> void:
	var box: VBoxContainer
	if detail_only:
		box = host as VBoxContainer
	else:
		box = _make_inspector_box(host)
	if box == null:
		return
	var lights := object_editor.object_data.get_lights() if object_editor and object_editor.object_data else []

	if not detail_only:
		_part_anim_list = null
		_materials_list = null
		_material_paste_button = null

		var list_panel := VBoxContainer.new()
		list_panel.name = "LightListPanel"
		list_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		box.add_child(list_panel)

		var summary := Label.new()
		summary.name = "LightListSummary"
		summary.theme_type_variation = &"Muted"
		summary.text = "%d lights" % lights.size()
		list_panel.add_child(summary)

		var list := ItemList.new()
		list.name = "ObjectLightsList"
		list.custom_minimum_size = Vector2(0, 360)
		list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		list.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_refresh_lights_list(list, lights)
		list_panel.add_child(list)
		_lights_list = list

		if lights.is_empty():
			_light_selected_index = -1
		else:
			_light_selected_index = clampi(_light_selected_index, 0, lights.size() - 1)
			list.select(_light_selected_index)

		list.item_selected.connect(func(index: int) -> void:
			_light_selected_index = index
			_rebuild_object_detail_dock()
		)
		return

	var detail_box := VBoxContainer.new()
	detail_box.name = "LightDetailPanel"
	detail_box.add_theme_constant_override("separation", 8)
	detail_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(detail_box)

	if lights.is_empty():
		var empty := Label.new()
		empty.name = "LightDetailEmpty"
		empty.theme_type_variation = &"Muted"
		empty.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		empty.text = "Select a light."
		detail_box.add_child(empty)
		return

	_light_selected_index = clampi(_light_selected_index, 0, lights.size() - 1)

	var selected := {"index": _light_selected_index}
	var syncing := {"value": false}

	var color_heading := Label.new()
	color_heading.theme_type_variation = &"Heading"
	color_heading.text = "Color"
	detail_box.add_child(color_heading)

	var start_color_row := _add_detail_field(detail_box, "Start color")
	var start_color := ColorPickerButton.new()
	start_color.name = "LightStartColor"
	start_color.custom_minimum_size = Vector2(0, 34)
	start_color.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	start_color_row.add_child(start_color)

	var end_color_row := _add_detail_field(detail_box, "End color")
	var end_color := ColorPickerButton.new()
	end_color.name = "LightEndColor"
	end_color.custom_minimum_size = Vector2(0, 34)
	end_color.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	end_color_row.add_child(end_color)

	var falloff_heading := Label.new()
	falloff_heading.theme_type_variation = &"Heading"
	falloff_heading.text = "Falloff"
	detail_box.add_child(falloff_heading)

	var attenuation_start := _add_detail_spin_row(detail_box, "LightAttenuationStart", "Atten start", 0, 100000, 0.1)
	var attenuation_end := _add_detail_spin_row(detail_box, "LightAttenuationEnd", "Atten end", 0, 100000, 0.1)
	var falloff := _add_detail_spin_row(detail_box, "LightFalloff", "Falloff", 0, 255, 1)

	var animation_heading := Label.new()
	animation_heading.theme_type_variation = &"Heading"
	animation_heading.text = "Animation"
	detail_box.add_child(animation_heading)

	var style := _add_detail_spin_row(detail_box, "LightStyle", "Style", 0, 255, 1)
	var phase := _add_detail_spin_row(detail_box, "LightPhase", "Phase", 0, 255, 1)
	var rate := _add_detail_spin_row(detail_box, "LightRate", "Rate", 0, 65535, 1)

	var output_heading := Label.new()
	output_heading.theme_type_variation = &"Heading"
	output_heading.text = "Output"
	detail_box.add_child(output_heading)

	var positive_flags_row := VBoxContainer.new()
	positive_flags_row.name = "LightPositiveFlags"
	positive_flags_row.add_theme_constant_override("separation", 2)
	positive_flags_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	detail_box.add_child(positive_flags_row)
	var draw_corona := CheckBox.new()
	draw_corona.name = "LightDrawCorona"
	draw_corona.text = "Draw corona"
	positive_flags_row.add_child(draw_corona)
	var light_terrain := CheckBox.new()
	light_terrain.name = "LightTerrain"
	light_terrain.text = "Light terrain"
	positive_flags_row.add_child(light_terrain)
	var light_objects := CheckBox.new()
	light_objects.name = "LightObjects"
	light_objects.text = "Light objects"
	positive_flags_row.add_child(light_objects)

	var flags_row := HBoxContainer.new()
	flags_row.name = "LightDisableRawRow"
	flags_row.visible = false
	flags_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	detail_box.add_child(flags_row)
	var disable_corona := CheckBox.new()
	disable_corona.name = "LightDisableCorona"
	disable_corona.text = "No corona"
	flags_row.add_child(disable_corona)
	var disable_terrain := CheckBox.new()
	disable_terrain.name = "LightDisableTerrain"
	disable_terrain.text = "No terrain"
	flags_row.add_child(disable_terrain)
	var disable_objects := CheckBox.new()
	disable_objects.name = "LightDisableObjects"
	disable_objects.text = "No objects"
	flags_row.add_child(disable_objects)

	var light_info := func(index: int) -> Dictionary:
		if object_editor == null or object_editor.object_data == null:
			return {}
		if object_editor.object_data.has_method("get_light_info"):
			return object_editor.object_data.get_light_info(index)
		return lights[index] if index >= 0 and index < lights.size() else {}

	var set_field := func(key: String, value: Variant) -> void:
		if syncing["value"] or selected["index"] < 0:
			return
		_light_selected_index = int(selected["index"])
		if object_editor.object_data.has_method("set_light_field"):
			object_editor.object_data.set_light_field(selected["index"], key, value)
		elif key == "color_start" or key == "color_end":
			object_editor.object_data.set_light_colors(selected["index"], start_color.color, end_color.color)
		_refresh_lights_left_list()

	var sync := func(index: int) -> void:
		syncing["value"] = true
		selected["index"] = index
		if index < 0 or index >= lights.size():
			syncing["value"] = false
			return
		var info: Dictionary = light_info.call(index)
		start_color.color = info.get("color_start", Color.WHITE)
		end_color.color = info.get("color_end", Color.WHITE)
		attenuation_start.value = float(info.get("atten_start", info.get("attenuation_start", 0.0)))
		attenuation_end.value = float(info.get("atten_end", info.get("attenuation_end", 0.0)))
		falloff.value = float(info.get("falloff_deg", info.get("falloff", 0.0)))
		style.value = int(info.get("colorgen_style", info.get("style", 0)))
		phase.value = int(info.get("colorgen_phase", info.get("phase", 0)))
		rate.value = int(info.get("colorgen_rate", info.get("rate", 0)))
		var corona_disabled := bool(info.get("disable_corona", (int(info.get("flags", 0)) & 0x01) != 0))
		var terrain_disabled := bool(info.get("disable_lightterrain", (int(info.get("flags", 0)) & 0x02) != 0))
		var objects_disabled := bool(info.get("disable_lightobjects", (int(info.get("flags", 0)) & 0x04) != 0))
		draw_corona.button_pressed = not corona_disabled
		light_terrain.button_pressed = not terrain_disabled
		light_objects.button_pressed = not objects_disabled
		disable_corona.button_pressed = corona_disabled
		disable_terrain.button_pressed = terrain_disabled
		disable_objects.button_pressed = objects_disabled
		syncing["value"] = false

	start_color.color_changed.connect(func(color: Color) -> void: set_field.call("color_start", color))
	end_color.color_changed.connect(func(color: Color) -> void: set_field.call("color_end", color))
	attenuation_start.value_changed.connect(func(value: float) -> void: set_field.call("atten_start", value))
	attenuation_end.value_changed.connect(func(value: float) -> void: set_field.call("atten_end", value))
	falloff.value_changed.connect(func(value: float) -> void: set_field.call("falloff_deg", value))
	style.value_changed.connect(func(value: float) -> void: set_field.call("colorgen_style", int(value)))
	phase.value_changed.connect(func(value: float) -> void: set_field.call("colorgen_phase", int(value)))
	rate.value_changed.connect(func(value: float) -> void: set_field.call("colorgen_rate", int(value)))
	draw_corona.toggled.connect(func(value: bool) -> void: set_field.call("disable_corona", not value))
	light_terrain.toggled.connect(func(value: bool) -> void: set_field.call("disable_lightterrain", not value))
	light_objects.toggled.connect(func(value: bool) -> void: set_field.call("disable_lightobjects", not value))
	disable_corona.toggled.connect(func(value: bool) -> void: set_field.call("disable_corona", value))
	disable_terrain.toggled.connect(func(value: bool) -> void: set_field.call("disable_lightterrain", value))
	disable_objects.toggled.connect(func(value: bool) -> void: set_field.call("disable_lightobjects", value))
	if not lights.is_empty():
		sync.call(_light_selected_index)


func _make_inspector_box(host: Control) -> VBoxContainer:
	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 10)
	margin.add_theme_constant_override("margin_top", 10)
	margin.add_theme_constant_override("margin_right", 10)
	margin.add_theme_constant_override("margin_bottom", 10)
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.size_flags_vertical = Control.SIZE_EXPAND_FILL
	host.add_child(margin)

	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	margin.add_child(scroll)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(box)
	return box
