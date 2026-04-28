extends GutTest

# Particle workspace tests — mirror the terrain workstation tests in shape.
# Verifies the workspace registers in the rail, exposes its three workflows,
# loads a fixture .ptl through the workspace adapter, and propagates dirty
# state when an inspector edits a field.

const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")
const ParticleWorkspaceScript = preload("res://modtools/editor/particle_workspace.gd")
const ParticleEditorScript = preload("res://modtools/particle/particle_editor.gd")
const ParticleEffectInspectorScript = preload("res://modtools/particle/inspectors/effect_inspector.gd")
const ParticleDefInspectorScript = preload("res://modtools/particle/inspectors/particle_inspector.gd")
const ParticleTableInspectorScript = preload("res://modtools/particle/inspectors/table_inspector.gd")


func _fixture(name: String) -> String:
	return ProjectSettings.globalize_path("res://../fixtures/particle/%s" % name)


func _find_inspector_of_type(root: Node, script: Script) -> Node:
	if root.get_script() == script:
		return root
	for child in root.get_children():
		var found := _find_inspector_of_type(child, script)
		if found != null:
			return found
	return null


func test_workstation_exposes_particles_workspace() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var rail: HBoxContainer = workstation.get_node("%WorkspaceRail")
	var found_label := false
	for child in rail.get_children():
		if (child as Button).text == "Particles":
			found_label = true
			break
	assert_true(found_label, "Particles workspace should appear in the rail.")
	var adapter = workstation._workspaces[EditorWorkstationScript.Workspace.PARTICLE]
	assert_not_null(adapter, "Particle adapter should be instantiated by _ensure_workspaces().")
	assert_eq(adapter.get_workspace_id(), "particle", "Adapter should report the particle id.")


func test_particles_workspace_exposes_three_workflows() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var mode_rail: VBoxContainer = workstation.get_node("%ModeRail")
	assert_true(mode_rail.visible, "Workflow rail should be visible while the particle workspace is active.")
	assert_eq(mode_rail.get_child_count(), 3, "Particle workspace should expose three workflows.")
	assert_eq((mode_rail.get_child(0) as Button).text, "Effects")
	assert_eq((mode_rail.get_child(1) as Button).text, "Particles")
	assert_eq((mode_rail.get_child(2) as Button).text, "Tables")


func test_particles_workspace_actions_listed() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var actions_host: VBoxContainer = workstation.get_node("%WorkspaceActionsHost")
	var labels: Array = []
	for child in actions_host.get_children():
		if child is Button:
			labels.append((child as Button).text)
	assert_eq(labels, ["New PTL", "Open PTL...", "Save PTL", "Save PTL As..."],
			"Particle workspace should expose new/open/save/save-as without a separate export.")


func test_open_fixture_loads_effects_and_particles() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation._workspaces[EditorWorkstationScript.Workspace.PARTICLE]
	var err: Error = adapter.open_file(_fixture("buildup.ptl"))
	assert_eq(err, OK, "buildup.ptl should load via the workspace adapter.")
	assert_eq(adapter.particle_editor.effect_count(), 1, "buildup has one effectdef.")
	assert_eq(adapter.particle_editor.particle_count(), 1, "buildup has one particledef.")
	assert_eq(adapter.particle_editor.table_count(), 0, "buildup has no tabledefs.")
	assert_false(adapter.particle_editor.is_dirty, "Newly loaded document is clean.")


func test_workflow_swap_mounts_correct_inspector() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation._workspaces[EditorWorkstationScript.Workspace.PARTICLE]
	adapter.open_file(_fixture("30MM.ptl"))
	var inspector_host: Control = workstation.get_node("%InspectorHost")

	# Default workflow is PARTICLES.
	workstation._set_workflow(ParticleWorkspaceScript.Workflow.PARTICLES, true)
	await get_tree().process_frame
	assert_not_null(_find_inspector_of_type(inspector_host, ParticleDefInspectorScript),
			"Particles workflow should mount the particle inspector.")

	workstation._set_workflow(ParticleWorkspaceScript.Workflow.EFFECTS, true)
	await get_tree().process_frame
	assert_not_null(_find_inspector_of_type(inspector_host, ParticleEffectInspectorScript),
			"Effects workflow should mount the effect inspector.")

	workstation._set_workflow(ParticleWorkspaceScript.Workflow.TABLES, true)
	await get_tree().process_frame
	assert_not_null(_find_inspector_of_type(inspector_host, ParticleTableInspectorScript),
			"Tables workflow should mount the table inspector.")


func test_property_edit_marks_document_dirty() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation._workspaces[EditorWorkstationScript.Workspace.PARTICLE]
	adapter.open_file(_fixture("buildup.ptl"))
	assert_false(adapter.particle_editor.is_dirty, "Fresh-loaded document is clean.")

	# Touch a field on the selected particle directly + mark dirty (mirrors what
	# the inspector does on a SpinBox value_changed). After that, save action
	# should be enabled.
	var particle: NovaParticleDef = adapter.particle_editor.current_particle
	assert_not_null(particle, "Open should pre-select the first particle.")
	particle.set_emit_dur(particle.get_emit_dur() + 1.0)
	adapter.particle_editor.mark_dirty()
	assert_true(adapter.particle_editor.is_dirty, "Edits should mark the document dirty.")
	assert_true(adapter.can_save(), "Dirty + path => save action available.")
