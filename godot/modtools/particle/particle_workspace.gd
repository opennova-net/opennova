class_name ParticleEditorWorkspace
extends EditorWorkspace

# Workspace adapter for the .ptl particle editor. Owns a ParticleEditor
# document model and mounts the blueprint screen (node graph + live preview)
# into the shell's viewport lane.
#
# Engine references for the simulation surfaced here:
#   CParticleDef_ParseFromConfigMap @ 0x5ed210  (def hydration)
#   CParticleEmitter_AdvanceFrame   @ 0x5e6570  (per-frame sim)
#   CParticleEffectDef_WriteToFile  @ 0x5e0fe0  (effectdef writer)

const ParticleEditorScript = preload("res://modtools/particle/particle_editor.gd")
const ParticleBlueprintScreenScript = preload("res://modtools/particle/blueprint/particle_blueprint_screen.gd")
const EffectInspectorScene = preload("res://modtools/particle/inspectors/effect_inspector.tscn")
const TableInspectorScene = preload("res://modtools/particle/inspectors/table_inspector.tscn")
const EffectInspectorScript = preload("res://modtools/particle/inspectors/effect_inspector.gd")
const ParticleInspectorScript = preload("res://modtools/particle/inspectors/particle_inspector.gd")
const TableInspectorScript = preload("res://modtools/particle/inspectors/table_inspector.gd")

enum Workflow { EFFECTS, PARTICLES, TABLES }

var particle_editor: ParticleEditor
var _screen: ParticleBlueprintScreen
var _preview: ParticlePreview
var _active_workflow: int = Workflow.PARTICLES
var _last_open_dir: String = ""
var _last_save_failure_message: String = ""


func _init() -> void:
	particle_editor = ParticleEditorScript.new()


func mount_viewport(mount: Control) -> void:
	if mount == null:
		return
	if _screen == null:
		# The blueprint screen owns the node graph + the live preview. We keep a
		# direct reference to the inner preview so all selection/preview plumbing
		# (and live-refresh wiring) works unchanged.
		_screen = ParticleBlueprintScreenScript.new()
		_screen.name = "ParticleBlueprint"
		_screen.set_anchors_preset(Control.PRESET_FULL_RECT)
		_screen.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_screen.size_flags_vertical = Control.SIZE_EXPAND_FILL
		mount.add_child(_screen)  # builds the GraphEdit + preview in _ready
		_screen.set_anchors_preset(Control.PRESET_FULL_RECT)
		_screen.set_workspace(self)
		_screen.set_particle_editor(particle_editor)
		_preview = _screen.get_preview()
		# Debounced live refresh: inspector edits ask the preview to re-apply the
		# current selection so changes show without a manual restart.
		if particle_editor != null and _preview != null \
				and not particle_editor.preview_refresh_requested.is_connected(_preview.request_live_refresh):
			particle_editor.preview_refresh_requested.connect(_preview.request_live_refresh)
	elif _screen.get_parent() == null:
		mount.add_child(_screen)
		_screen.set_anchors_preset(Control.PRESET_FULL_RECT)
	_apply_current_selection_to_preview()


func unmount_viewport(_released: Control) -> void:
	if _screen == null:
		return
	if _screen.get_parent() != null:
		_screen.get_parent().remove_child(_screen)


func release_viewport() -> void:
	if _screen == null:
		return
	if _screen.get_parent() != null:
		_screen.get_parent().remove_child(_screen)
	_screen.free()
	_screen = null
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


func get_workspace_tooltip() -> String:
	return "Author particle effects (.ptl): explosions, smoke, muzzle flashes, water spray"


func get_project_title() -> String:
	if particle_editor == null:
		return "Particles"
	if particle_editor.current_path.is_empty():
		return "Particles — Untitled"
	return "Particles — " + particle_editor.current_path.get_file()


func get_status_tool() -> String:
	return "Particles"


# Opt into the shell's Show grid / Show axes toggles for the 3D preview.
func shows_view_guides() -> bool:
	return true


func get_viewport_camera() -> Camera3D:
	return _screen.get_viewport_camera() if _screen != null else null


func set_grid_visible(value: bool) -> void:
	if _preview != null:
		_preview.set_grid_visible(value)


func set_axes_visible(value: bool) -> void:
	if _preview != null:
		_preview.set_axes_visible(value)


func get_status_context() -> String:
	if particle_editor == null:
		return ""
	return "%d effects · %d particles · %d tables" % [
		particle_editor.effect_count(),
		particle_editor.particle_count(),
		particle_editor.table_count(),
	]


# The domain document the EditorWorkspace base derives dirty state from
# (ParticleEditor exposes an is_dirty bool; no edit history yet, so undo/redo
# stay unavailable).
func get_editor_document() -> Object:
	return particle_editor


func _build_inspector_defs() -> Array:
	# The shell renders the rail from these InspectorDef rows (id/label/tooltip)
	# and instantiates the inspector via build_workflow_inspector() below, which
	# loads the .tscn scenes and wires them to this workspace + the editor model.
	return [
		InspectorDef.make(Workflow.EFFECTS, "Effects", "Browse and edit effect entries", EffectInspectorScript),
		InspectorDef.make(Workflow.PARTICLES, "Particles", "Browse and edit particle entries; live preview", ParticleInspectorScript),
		InspectorDef.make(Workflow.TABLES, "Tables", "Draw and assign curve tables", TableInspectorScript),
	]


func get_active_workflow_id() -> int:
	return _active_workflow


func activate_workflow(workflow_id: int) -> void:
	_active_workflow = workflow_id


## Programmatic workflow switch from workspace UI (the blueprint selecting a
## node of a different kind): activates the workflow, then asks the shell to
## re-sync its rail + inspector to match.
func select_workflow(workflow_id: int) -> void:
	activate_workflow(workflow_id)
	_sync_shell_workflow()


func build_workflow_inspector(workflow_id: int, mount: Control) -> void:
	if particle_editor == null:
		return
	var inspector: Control
	match workflow_id:
		Workflow.EFFECTS:
			inspector = EffectInspectorScene.instantiate()
		Workflow.PARTICLES:
			# Code-first inspector (no companion .tscn); see particle_inspector.gd.
			inspector = ParticleInspectorScript.new()
		Workflow.TABLES:
			inspector = TableInspectorScene.instantiate()
		_:
			return
	mount.add_child(inspector)
	if inspector.has_method("set_particle_editor"):
		inspector.set_particle_editor(particle_editor)
	if inspector.has_method("set_workspace"):
		inspector.set_workspace(self)
	# Refresh preview after inspector mounts (selection may change immediately).
	_apply_current_selection_to_preview()


func can_new() -> bool:
	return particle_editor != null


func get_new_action_label() -> String:
	return "New PTL"


func new_current() -> Error:
	if particle_editor == null:
		return ERR_UNAVAILABLE
	var make_new := func() -> void:
		particle_editor.new_document()
		_apply_current_selection_to_preview()
	if _prompt_dirty_guard(make_new):
		return OK
	make_new.call()
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
	var open_it := func() -> void:
		var err := _open_file_unchecked(path)
		if err != OK:
			_notify_status("Open failed for %s (error %d)." % [path.get_file(), err], &"error")
	if _prompt_dirty_guard(open_it):
		return OK
	return _open_file_unchecked(path)


func _open_file_unchecked(path: String) -> Error:
	var err: int
	var vfs := _vfs_root_for_open(path)
	if vfs != null:
		err = particle_editor.open_bytes(vfs.read_file(path), _vfs_display_path(vfs, path))
	else:
		err = particle_editor.open_file(path)
	if err == OK:
		_last_open_dir = particle_editor.current_path.get_base_dir()
		_apply_current_selection_to_preview()
	return err


# New/Open replace the single PTL document. When it is dirty, hand ownership
# of that continuation to the shell's shared Save/Discard/Cancel prompt.
func _prompt_dirty_guard(run: Callable) -> bool:
	if particle_editor == null or not particle_editor.is_dirty:
		return false
	if editor_shell == null or not editor_shell.has_method("prompt_unsaved_for"):
		return false
	var shell: Object = editor_shell
	var workspace: EditorWorkspace = self
	shell.prompt_unsaved_for(
		func() -> void: shell.save_then(workspace, run),
		run)
	return true


func can_save() -> bool:
	return particle_editor != null and particle_editor.can_save() and particle_editor.is_dirty


func can_save_as() -> bool:
	return particle_editor != null


func get_save_action_label() -> String:
	return "Save PTL"


func get_save_as_action_label() -> String:
	return "Save PTL As..."


func save_current() -> Error:
	_last_save_failure_message = ""
	if particle_editor == null:
		return ERR_UNAVAILABLE
	if particle_editor.current_path.is_empty():
		return ERR_INVALID_PARAMETER  # shell will fall back to Save As
	if not _passes_presave_validation():
		return ERR_INVALID_DATA  # message already shown; shell must not continue
	var err: int = particle_editor.save_current()
	if err == OK:
		_notify_status("Saved %s." % particle_editor.current_path.get_file(), &"success")
	return err


func save_as(dir_path: String) -> Error:
	return save_as_file(dir_path.path_join(get_save_file_dialog_default_name()))


func uses_save_file_dialog() -> bool:
	return true


func get_save_file_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.ptl,*.PTL ; NovaLogic Particle"])


func get_save_file_dialog_default_name() -> String:
	if particle_editor != null and not particle_editor.current_path.is_empty():
		return particle_editor.current_path.get_file()
	return "untitled.ptl"


func save_as_file(path: String) -> Error:
	_last_save_failure_message = ""
	if particle_editor == null:
		return ERR_UNAVAILABLE
	if not _passes_presave_validation():
		return ERR_INVALID_DATA
	var err: int = particle_editor.save_to_path(path)
	if err == OK:
		_last_open_dir = path.get_base_dir()
		_notify_status("Saved %s." % path.get_file(), &"success")
	return err


func get_save_failure_message(_error: Error) -> String:
	return _last_save_failure_message


# Runs ParticleEditor.validate() before a write. Returns false (and shows the
# blocking message) when there is a structural error; surfaces the first warning
# but returns true otherwise. The save handlers return ERR_INVALID_DATA on a
# block so dirty-close/open continuations cannot run as if a write succeeded.
func _passes_presave_validation() -> bool:
	if particle_editor == null:
		return false
	var issues: Array = particle_editor.validate()
	var errors: Array = issues.filter(func(i): return i.get("severity") == "error")
	if not errors.is_empty():
		var msg: String = errors[0].get("message", "Cannot save: invalid particle data.")
		if errors.size() > 1:
			msg += " (+%d more issue%s)" % [errors.size() - 1, "" if errors.size() == 2 else "s"]
		_last_save_failure_message = msg
		_notify_status(msg, &"error")
		return false
	var warnings: Array = issues.filter(func(i): return i.get("severity") == "warning")
	if not warnings.is_empty():
		_notify_status(warnings[0].get("message", ""), &"warn")
	return true


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


# --- CRUD passthroughs the inspectors + blueprint call. Each mutates the model
# then re-applies the current selection to the preview so edits show live. ---

func add_particle() -> NovaParticleDef:
	if particle_editor == null:
		return null
	var p := particle_editor.add_particle()
	_apply_current_selection_to_preview()
	return p


func duplicate_particle(p: NovaParticleDef) -> NovaParticleDef:
	if particle_editor == null:
		return null
	var dup := particle_editor.duplicate_particle(p)
	_apply_current_selection_to_preview()
	return dup


func remove_particle(p: NovaParticleDef) -> void:
	if particle_editor == null:
		return
	particle_editor.remove_particle(p)
	_apply_current_selection_to_preview()


func add_effect() -> NovaParticleEffect:
	if particle_editor == null:
		return null
	var e := particle_editor.add_effect()
	_apply_current_selection_to_preview()
	return e


func duplicate_effect(e: NovaParticleEffect) -> NovaParticleEffect:
	if particle_editor == null:
		return null
	var dup := particle_editor.duplicate_effect(e)
	_apply_current_selection_to_preview()
	return dup


func remove_effect(e: NovaParticleEffect) -> void:
	if particle_editor == null:
		return
	particle_editor.remove_effect(e)
	_apply_current_selection_to_preview()


func add_table() -> NovaParticleTable:
	if particle_editor == null:
		return null
	var t := particle_editor.add_table()
	return t


func duplicate_table(t: NovaParticleTable) -> NovaParticleTable:
	if particle_editor == null:
		return null
	return particle_editor.duplicate_table(t)


func remove_table(t: NovaParticleTable) -> void:
	if particle_editor == null:
		return
	particle_editor.remove_table(t)


func add_graphic_layer(p: NovaParticleDef) -> int:
	if particle_editor == null:
		return -1
	var idx := particle_editor.add_graphic_layer(p)
	_refresh_preview_if_current(p)
	return idx


func remove_graphic_layer(p: NovaParticleDef, slot: int) -> void:
	if particle_editor == null:
		return
	particle_editor.remove_graphic_layer(p, slot)
	_refresh_preview_if_current(p)


func effect_add_pdef(effect: NovaParticleEffect, pdef_id: String) -> void:
	if particle_editor == null:
		return
	particle_editor.effect_add_pdef(effect, pdef_id)
	if particle_editor.current_effect == effect:
		_apply_current_selection_to_preview()


func effect_remove_pdef(effect: NovaParticleEffect, pdef_id: String) -> void:
	if particle_editor == null:
		return
	particle_editor.effect_remove_pdef(effect, pdef_id)
	if particle_editor.current_effect == effect:
		_apply_current_selection_to_preview()


func set_child_id(p: NovaParticleDef, child: String) -> void:
	if particle_editor == null:
		return
	particle_editor.set_child_id(p, child)


func assign_curve(p: NovaParticleDef, field: String, table_id: String) -> void:
	if particle_editor == null:
		return
	particle_editor.assign_curve(p, field, table_id)
	_refresh_preview_if_current(p)


func _refresh_preview_if_current(p: NovaParticleDef) -> void:
	if particle_editor != null and particle_editor.current_particle == p:
		_apply_current_selection_to_preview()
