class_name ObjectEditorWorkspace
extends EditorWorkspace

const ObjectEditorScript = preload("res://modtools/object/object_editor.gd")
const ObjectPreviewScript = preload("res://modtools/object/object_preview.gd")
const PreviewInspectorScript = preload("res://modtools/object/ui/inspectors/preview_inspector.gd")
const LodsInspectorScript = preload("res://modtools/object/ui/inspectors/lods_inspector.gd")
const LightsInspectorScript = preload("res://modtools/object/ui/inspectors/lights_inspector.gd")
const MaterialsInspectorScript = preload("res://modtools/object/ui/inspectors/materials_inspector.gd")
const PartAnimsInspectorScript = preload("res://modtools/object/ui/inspectors/part_anims_inspector.gd")

enum Workflow { PREVIEW, MATERIALS, PARTS, LIGHTS, LODS }

# Workflow inspectors are declared as typed InspectorDef rows in _build_inspector_defs().

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
var _mount: ViewportMount
var _asset_dock_host: Control
var _object_detail_dock: Control
var _inspectors: Dictionary = {}
var _export_update_mask: int = OED_UPDATE_NONE


func set_editor_shell(value: Node) -> void:
	super.set_editor_shell(value)
	_ensure_object_editor()


func set_environment_editor(value) -> void:
	SignalRebind.rebind(environment_editor, value, &"environment_changed", _on_environment_editor_changed)
	environment_editor = value
	_apply_environment_to_preview()


func bind_to_editor(value: Node) -> void:
	var env = value.get_environment_editor() if value != null and value.has_method("get_environment_editor") else null
	set_environment_editor(env)


func get_workspace_tooltip() -> String:
	return "Edit object projects, materials, LODs, lights, and 3DI export."


func activate() -> void:
	_ensure_object_editor()


func deactivate() -> void:
	pass


func _ensure_mount() -> ViewportMount:
	if _mount == null:
		_mount = ViewportMount.new(&"ObjectPreview", _create_preview)
	return _mount


# Factory for the shared ViewportMount: news the ObjectPreview and wires its
# object data + environment once on creation. ViewportMount owns the name /
# anchors / parenting lifecycle (matching terrain_workspace + mission_workspace).
func _create_preview() -> Control:
	_preview = ObjectPreviewScript.new()
	_preview.set_object_data(object_editor.object_data if object_editor else null)
	_apply_environment_to_preview()
	return _preview


func mount_viewport(host: Control) -> void:
	if host == null:
		return
	_ensure_object_editor()
	_ensure_mount().mount(host)


func unmount_viewport(_host: Control) -> void:
	if _mount != null:
		_mount.unmount()


func release_viewport() -> void:
	if _mount != null:
		_mount.release()
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


func get_active_workflow_id() -> int:
	return _active_workflow_id


func activate_workflow(workflow_id: int) -> void:
	_active_workflow_id = workflow_id
	_sync_object_detail_dock_mount()


func build_workflow_inspector(workflow_id: int, host: Control) -> void:
	_ensure_object_editor()
	_active_workflow_id = workflow_id
	var inspector := _inspector_for(workflow_id)
	if inspector != null:
		inspector.build_main(host)
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
	var inspector := _inspector_for(Workflow.PREVIEW)
	if inspector != null:
		inspector.build_main(host)


func _build_inspector_defs() -> Array:
	return [
		InspectorDef.make(Workflow.PREVIEW, "Preview", "View the object with fixed editor lighting.", PreviewInspectorScript),
		InspectorDef.make(Workflow.MATERIALS, "Materials", "Edit material shader tags, textures, and alpha.", MaterialsInspectorScript),
		InspectorDef.make(Workflow.PARTS, "Part Anims", "Inspect and edit PANM part animation entries.", PartAnimsInspectorScript),
		InspectorDef.make(Workflow.LIGHTS, "Lights", "Inspect and edit object light colors.", LightsInspectorScript),
		InspectorDef.make(Workflow.LODS, "LODs", "Bind ASE scenes and edit project LOD settings.", LodsInspectorScript),
	]


func _ensure_inspectors() -> void:
	for def in _ensure_inspector_defs():
		var inspector_def := def as InspectorDef
		if _inspectors.get(inspector_def.id) == null:
			_inspectors[inspector_def.id] = inspector_def.inspector_script.new(self)


func _inspector_for(workflow_id: int) -> WorkflowInspector:
	_ensure_inspectors()
	var inspector = _inspectors.get(workflow_id)
	if inspector == null:
		inspector = _inspectors.get(Workflow.PREVIEW)
	return inspector


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
	var active_inspector := _inspector_for(_active_workflow_id)
	if active_inspector != null and active_inspector.has_detail():
		active_inspector.refresh()
	else:
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


func _active_workflow_uses_detail_dock() -> bool:
	var inspector := _inspector_for(_active_workflow_id)
	return inspector != null and inspector.has_detail()


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

	var inspector := _inspector_for(_active_workflow_id)
	if inspector != null and inspector.has_detail():
		inspector.build_detail(box)
