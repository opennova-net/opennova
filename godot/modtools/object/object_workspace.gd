class_name ObjectEditorWorkspace
extends EditorWorkspace

const ObjectEditorScript = preload("res://modtools/object/object_editor.gd")
const ObjectPreviewScript = preload("res://modtools/object/object_preview.gd")
const PreviewInspectorScript = preload("res://modtools/object/ui/inspectors/preview_inspector.gd")
const LodsInspectorScript = preload("res://modtools/object/ui/inspectors/lods_inspector.gd")
const LightsInspectorScript = preload("res://modtools/object/ui/inspectors/lights_inspector.gd")
const MaterialsInspectorScript = preload("res://modtools/object/ui/inspectors/materials_inspector.gd")
const PartAnimsInspectorScript = preload("res://modtools/object/ui/inspectors/part_anims_inspector.gd")
const AnimsInspectorScript = preload("res://modtools/object/ui/inspectors/anims_inspector.gd")

# ANIMS is APPENDED (object_editor_test pins LODS == raw id 4 — never renumber);
# tab order comes from the defs array below, independent of these ids.
enum Workflow { PREVIEW, MATERIALS, PARTS, LIGHTS, LODS, ANIMS }

# Workflow inspectors are declared as typed InspectorDef rows in _build_inspector_defs().

# Aliases of the NovaObjectData binding, single-sourced from libs/oed
# (OED_UPDATE_*) — ENG-4.
const OED_UPDATE_NONE := NovaObjectData.UPDATE_NONE
const OED_UPDATE_MTRL := NovaObjectData.UPDATE_MTRL
const OED_UPDATE_LGHT := NovaObjectData.UPDATE_LGHT
const OED_UPDATE_PANM := NovaObjectData.UPDATE_PANM
const OED_UPDATE_ALL := NovaObjectData.UPDATE_ALL
# Maximum value of an authored 16-bit unsigned field (for example, light rate).
const U16_VALUE_MAX := 65535
# The retail runtime CTRL bus stores signed dwords. Preview authoring must expose
# the same complete range; several dedicated writers use values outside the
# usual 0..0x10000 animation interval.
const CTRL_VALUE_MIN := -2147483648
const CTRL_VALUE_MAX := 2147483647

var object_editor: ObjectEditor
var environment_editor
var _active_workflow_id: int = Workflow.PREVIEW
var _preview: ObjectPreview
var _mount: ViewportMount
# The asset-dock detail pane, mounted by the framework's DetailDockMount: it owns
# the lazy panel, the conditional mount, and the rebuild-only-on-real-change
# policy (a routine editor-state sync — e.g. a time-of-day drag — must never
# tear the pane down; that was the TOD lag).
var _detail_dock: DetailDockMount
var _export_update_mask: int = OED_UPDATE_NONE
# Preview guide visibility, driven by the shell's View settings. Stored here so a
# re-mounted preview (ViewportMount rebuilds it) inherits the current choice.
var _grid_visible := true
var _axes_visible := true


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
	_connect_focus_bracket()


func deactivate() -> void:
	_disconnect_focus_bracket()


# --- Undo session coalescing (B5) ----------------------------------------------
# While the workspace is active, viewport focus drives the document's
# begin/commit bracket: focus entering an editable control opens an editing
# burst, leaving it folds the burst into ONE equal-gated undo step — so typing
# or dragging in any inspector field coalesces without per-widget wiring. The
# shadow-step funnel (B4) skips recording while a burst is open. A color-drag
# whose popup closes without a focus change folds on the next flush (undo and
# push_undo_step both flush first), preserving one-step-per-gesture.

var _focus_viewport: Viewport = null


func _connect_focus_bracket() -> void:
	var viewport: Viewport = null
	if object_editor != null and object_editor.is_inside_tree():
		viewport = object_editor.get_viewport()
	if viewport == null or viewport == _focus_viewport:
		return
	_disconnect_focus_bracket()
	_focus_viewport = viewport
	viewport.gui_focus_changed.connect(_on_bracket_focus_changed)


func _disconnect_focus_bracket() -> void:
	if _focus_viewport != null and is_instance_valid(_focus_viewport) \
			and _focus_viewport.gui_focus_changed.is_connected(_on_bracket_focus_changed):
		_focus_viewport.gui_focus_changed.disconnect(_on_bracket_focus_changed)
	_focus_viewport = null
	if object_editor != null:
		object_editor.flush_edit()


func _is_bracket_control(control: Control) -> bool:
	return control is LineEdit or control is TextEdit \
			or control is SpinBox or control is ColorPickerButton


func _on_bracket_focus_changed(control: Control) -> void:
	if object_editor == null:
		return
	# Only the NEW focus owner arrives, so fold unconditionally first (commit
	# is equal-gated and inert without an open burst), then open a burst when
	# an editable control took the focus.
	object_editor.flush_edit()
	if control != null and _is_bracket_control(control):
		object_editor.begin_edit()


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
	_preview.set_grid_visible(_grid_visible)
	_preview.set_axes_visible(_axes_visible)
	_apply_environment_to_preview()
	return _preview


func mount_viewport(mount: Control) -> void:
	if mount == null:
		return
	_ensure_object_editor()
	_ensure_mount().mount(mount)


func unmount_viewport(_released: Control) -> void:
	if _mount != null:
		_mount.unmount()


func release_viewport() -> void:
	if _mount != null:
		_mount.release()
	_preview = null


func get_viewport_camera() -> Camera3D:
	if _preview != null:
		return _preview.get_editor_camera()
	return null


## The live ObjectPreview, or null before the viewport mounts / after release.
## Public surface for tools and the MCP object verbs — reaching for _preview
## from outside is a ratcheted private poke (ADR 0018).
func get_preview() -> ObjectPreview:
	return _preview if _preview != null and is_instance_valid(_preview) else null


func shows_view_guides() -> bool:
	return true


func set_grid_visible(value: bool) -> void:
	_grid_visible = value
	if _preview != null:
		_preview.set_grid_visible(value)


func set_axes_visible(value: bool) -> void:
	_axes_visible = value
	if _preview != null:
		_preview.set_axes_visible(value)


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
		Workflow.ANIMS:
			return "Anims"
		_:
			return "Object"


func get_status_context() -> String:
	return object_editor.get_status_context() if object_editor else "No object loaded"


func uses_asset_dock() -> bool:
	return _active_workflow_uses_detail_dock()


func set_asset_dock(dock: Control) -> void:
	_ensure_detail_dock().set_mount(dock)


func sync_asset_dock() -> void:
	_ensure_detail_dock().ensure_mounted()


func _ensure_detail_dock() -> DetailDockMount:
	if _detail_dock == null:
		_detail_dock = DetailDockMount.new(&"ObjectDetailDock", &"ObjectDetailDockBox",
			_active_workflow_uses_detail_dock,
			func(box: Control) -> void:
				var inspector := _inspector_for(_active_workflow_id)
				if inspector != null and inspector.has_detail():
					inspector.build_detail(box),
			func() -> int:
				return object_editor.object_data.get_instance_id() if object_editor != null and object_editor.object_data != null else 0)
	return _detail_dock


func get_active_workflow_id() -> int:
	return _active_workflow_id


func activate_workflow(workflow_id: int) -> void:
	_active_workflow_id = workflow_id
	_ensure_detail_dock().sync_mount()


func build_workflow_inspector(workflow_id: int, mount: Control) -> void:
	_ensure_object_editor()
	_active_workflow_id = workflow_id
	var inspector := _inspector_for(workflow_id)
	if inspector != null:
		inspector.build_main(mount)
	_ensure_detail_dock().sync_mount()


# The domain document the EditorWorkspace base derives undo/redo + dirty from.
func get_editor_document() -> Object:
	return object_editor


func can_new() -> bool:
	return true


func get_new_action_label() -> String:
	return "New Object"


func new_current() -> Error:
	_ensure_object_editor()
	object_editor.create_empty_object(true)
	_after_document_changed()
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
	var vfs := _vfs_root_for_open(path)
	var err := OK
	if vfs != null:
		err = object_editor.open_object_from_resource_root(vfs, path)
	else:
		err = object_editor.open_object(path)
	_after_document_changed()
	return err


## The mounted game resource root (VFS), if the shell provides one. Used by the preview
## to resolve a model's .adm/.bad animation files for the skeletal-animation smoke test.
func get_resource_root() -> NovaResourceRoot:
	return _resource_root()


## Open the shell's scoped file picker over an explicit file list (the object
## inspectors' .adm / arms-.3di pickers ask the workspace, not the shell).
## No-op without a shell (headless / tests).
func open_file_picker(title: String, files: PackedStringArray, on_pick: Callable) -> void:
	if editor_shell != null and editor_shell.has_method("open_file_picker"):
		editor_shell.open_file_picker(title, files, on_pick)


func add_lod_scene(path: String, lod_index: int = -1) -> Error:
	_ensure_object_editor()
	var err := object_editor.add_lod_scene(path, lod_index)
	_after_document_changed()
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


func build_inspector(mount: Control) -> void:
	var inspector := _inspector_for(Workflow.PREVIEW)
	if inspector != null:
		inspector.build_main(mount)


func _build_inspector_defs() -> Array:
	return [
		InspectorDef.make(Workflow.PREVIEW, "Preview", "View the object with fixed editor lighting.", PreviewInspectorScript),
		InspectorDef.make(Workflow.ANIMS, "Anims", "Load a body-animation set and play or scrub its clips.", AnimsInspectorScript),
		InspectorDef.make(Workflow.MATERIALS, "Materials", "Edit material shader tags, textures, and alpha.", MaterialsInspectorScript),
		InspectorDef.make(Workflow.PARTS, "Part Anims", "Inspect and edit PANM part animation entries.", PartAnimsInspectorScript),
		InspectorDef.make(Workflow.LIGHTS, "Lights", "Inspect and edit object light colors.", LightsInspectorScript),
		InspectorDef.make(Workflow.LODS, "LODs", "Bind ASE scenes and edit project LOD settings.", LodsInspectorScript),
	]


# Object inspectors take the workspace in their constructor.
func _instantiate_inspector(def: InspectorDef) -> Object:
	return def.inspector_script.new(self)


func _inspector_for(workflow_id: int) -> WorkflowInspector:
	var inspector := get_workflow_inspector(workflow_id)
	if inspector == null:
		inspector = get_workflow_inspector(Workflow.PREVIEW)
	return inspector


func _ensure_object_editor() -> void:
	if object_editor != null:
		return
	object_editor = ObjectEditorScript.new()
	object_editor.name = "ObjectEditor"
	_mount_under_shell(object_editor)
	object_editor.create_empty_object(false)
	object_editor.state_changed.connect(_after_document_changed)
	_sync_export_update_mask_from_dirty()


# Post-change fan-out: refresh the preview binding, the active inspector, and the
# shell chrome after any object-document mutation (state_changed lands here too).
func _after_document_changed() -> void:
	_sync_export_update_mask_from_dirty()
	if _preview != null and object_editor != null:
		if _preview.object_data != object_editor.object_data:
			_preview.set_object_data(object_editor.object_data)
		_apply_environment_to_preview()
	var active_inspector := _inspector_for(_active_workflow_id)
	if active_inspector != null and active_inspector.has_detail():
		active_inspector.refresh()
	else:
		_ensure_detail_dock().rebuild()
	_sync_shell()


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
	return int(object_editor.object_data.get_oed_dirty_mask()) & OED_UPDATE_ALL


func _active_workflow_uses_detail_dock() -> bool:
	var inspector := _inspector_for(_active_workflow_id)
	return inspector != null and inspector.has_detail()
