class_name ParticleEditorWorkspace
extends EditorWorkspace

# Workspace adapter for the .ptl particle editor. Owns a ParticleEditor
# document model and a ParticlePreview viewport that the shell mounts into its
# dedicated viewport lane.
#
# Engine references for the simulation surfaced here:
#   CParticleDef_ParseFromConfigMap @ 0x5ed210  (def hydration)
#   CParticleEmitter_AdvanceFrame   @ 0x5e6570  (per-frame sim)
#   CParticleEffectDef_WriteToFile  @ 0x5e0fe0  (effectdef writer)

const ParticleEditorScript = preload("res://modtools/particle/particle_editor.gd")
const ParticlePreviewScript = preload("res://modtools/particle/particle_preview.gd")
const EffectInspectorScene = preload("res://modtools/particle/inspectors/effect_inspector.tscn")
const ParticleInspectorScene = preload("res://modtools/particle/inspectors/particle_inspector.tscn")
const TableInspectorScene = preload("res://modtools/particle/inspectors/table_inspector.tscn")
const EffectInspectorScript = preload("res://modtools/particle/inspectors/effect_inspector.gd")
const ParticleInspectorScript = preload("res://modtools/particle/inspectors/particle_inspector.gd")
const TableInspectorScript = preload("res://modtools/particle/inspectors/table_inspector.gd")

enum Workflow { EFFECTS, PARTICLES, TABLES }

var particle_editor: ParticleEditor
var _preview: ParticlePreview
var _active_workflow: int = Workflow.PARTICLES
var _last_open_dir: String = ""


func _init() -> void:
	particle_editor = ParticleEditorScript.new()


func mount_viewport(host: Control) -> void:
	if host == null:
		return
	if _preview == null:
		_preview = ParticlePreviewScript.new()
		_preview.name = "ParticlePreview"
		_preview.set_anchors_preset(Control.PRESET_FULL_RECT)
		_preview.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_preview.size_flags_vertical = Control.SIZE_EXPAND_FILL
	if _preview.get_parent() == null:
		host.add_child(_preview)
		_preview.set_anchors_preset(Control.PRESET_FULL_RECT)
	_apply_current_selection_to_preview()


func unmount_viewport(_host: Control) -> void:
	if _preview == null:
		return
	if _preview.get_parent() != null:
		_preview.get_parent().remove_child(_preview)


func release_viewport() -> void:
	if _preview == null:
		return
	if _preview.get_parent() != null:
		_preview.get_parent().remove_child(_preview)
	_preview.free()
	_preview = null


func _apply_current_selection_to_preview() -> void:
	if _preview == null or particle_editor == null:
		return
	_preview.set_particle_file(particle_editor.particle_file)
	match _active_workflow:
		Workflow.EFFECTS:
			if particle_editor.current_effect != null and particle_editor.particle_file != null:
				_preview.set_effect(particle_editor.current_effect)
			else:
				_preview.clear_preview()
		Workflow.PARTICLES:
			if particle_editor.current_particle != null:
				_preview.set_particle_def(particle_editor.current_particle)
			else:
				_preview.clear_preview()
		_:
			_preview.clear_preview()


func get_workspace_id() -> String:
	return "particle"


func get_workspace_label() -> String:
	return "Particles"


func get_project_title() -> String:
	if particle_editor == null:
		return "Particles"
	if particle_editor.current_path.is_empty():
		return "Particles — Untitled"
	return "Particles — " + particle_editor.current_path.get_file()


func get_status_tool() -> String:
	return "Particles"


func get_status_context() -> String:
	if particle_editor == null:
		return ""
	return "%d effects · %d particles · %d tables" % [
		particle_editor.effect_count(),
		particle_editor.particle_count(),
		particle_editor.table_count(),
	]


func _build_inspector_defs() -> Array:
	# The shell renders the rail from these InspectorDef rows (id/label/tooltip)
	# and instantiates the inspector via build_workflow_inspector() below, which
	# loads the .tscn scenes and wires them to this workspace + the editor model.
	return [
		InspectorDef.make(Workflow.EFFECTS, "Effects", "Browse and edit [effectdef] entries", EffectInspectorScript),
		InspectorDef.make(Workflow.PARTICLES, "Particles", "Browse and edit [particledef] entries; live preview", ParticleInspectorScript),
		InspectorDef.make(Workflow.TABLES, "Tables", "Visualize [tabledef] curves", TableInspectorScript),
	]


func get_active_workflow_id() -> int:
	return _active_workflow


func activate_workflow(workflow_id: int) -> void:
	_active_workflow = workflow_id


func build_workflow_inspector(workflow_id: int, host: Control) -> void:
	if particle_editor == null:
		return
	var inspector: Control
	match workflow_id:
		Workflow.EFFECTS:
			inspector = EffectInspectorScene.instantiate()
		Workflow.PARTICLES:
			inspector = ParticleInspectorScene.instantiate()
		Workflow.TABLES:
			inspector = TableInspectorScene.instantiate()
		_:
			return
	host.add_child(inspector)
	if inspector.has_method("set_particle_editor"):
		inspector.set_particle_editor(particle_editor)
	if inspector.has_method("set_workspace"):
		inspector.set_workspace(self)
	# Refresh preview after inspector mounts (selection may change immediately).
	_apply_current_selection_to_preview()


func has_unsaved_changes() -> bool:
	return particle_editor != null and particle_editor.is_dirty


func can_new() -> bool:
	return particle_editor != null


func get_new_action_label() -> String:
	return "New PTL"


func new_current() -> Error:
	if particle_editor == null:
		return ERR_UNAVAILABLE
	particle_editor.new_document()
	_apply_current_selection_to_preview()
	return OK


func can_open() -> bool:
	return particle_editor != null


func get_open_action_label() -> String:
	return "Open PTL..."


func get_open_dialog_title() -> String:
	return "Open .ptl"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.ptl,*.PTL ; NovaLogic Particle"])


func get_open_dialog_dir() -> String:
	return _last_open_dir


func get_open_resource_kind() -> String:
	return "particle"


func get_current_resource_path() -> String:
	return particle_editor.current_path if particle_editor != null else ""


func open_file(path: String) -> Error:
	if particle_editor == null:
		return ERR_UNAVAILABLE
	var err: int = particle_editor.open_file(path)
	if err == OK:
		_last_open_dir = path.get_base_dir()
		_apply_current_selection_to_preview()
	return err


func can_save() -> bool:
	return particle_editor != null and particle_editor.can_save() and particle_editor.is_dirty


func can_save_as() -> bool:
	return particle_editor != null


func get_save_action_label() -> String:
	return "Save PTL"


func get_save_as_action_label() -> String:
	return "Save PTL As..."


func save_current() -> Error:
	if particle_editor == null:
		return ERR_UNAVAILABLE
	if particle_editor.current_path.is_empty():
		return ERR_INVALID_PARAMETER  # shell will fall back to Save As
	return particle_editor.save_current()


func save_as(dir_path: String) -> Error:
	if particle_editor == null:
		return ERR_UNAVAILABLE
	# Save dialog returns a directory; pick a default filename if the document
	# doesn't have one yet, otherwise reuse the existing leaf.
	var leaf: String = "untitled.ptl"
	if not particle_editor.current_path.is_empty():
		leaf = particle_editor.current_path.get_file()
	return particle_editor.save_to_path(dir_path.path_join(leaf))


func get_save_dialog_title() -> String:
	return "Choose where to save the .ptl"


func get_save_dialog_dir() -> String:
	if particle_editor != null and not particle_editor.current_path.is_empty():
		return particle_editor.current_path.get_base_dir()
	return _last_open_dir


# Selection passthroughs the inspectors call when the user clicks a row.
func select_effect(effect: NovaParticleEffect) -> void:
	if particle_editor == null:
		return
	particle_editor.select_effect(effect)
	_apply_current_selection_to_preview()


func select_particle(particle: NovaParticleDef) -> void:
	if particle_editor == null:
		return
	particle_editor.select_particle(particle)
	_apply_current_selection_to_preview()


func select_table(table: NovaParticleTable) -> void:
	if particle_editor == null:
		return
	particle_editor.select_table(table)
	if _preview != null:
		_preview.clear_preview()
