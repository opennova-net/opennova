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

const OUTPUT_DIR_NAME := "particle_editor_workstation_test"


func before_each() -> void:
	_cleanup_dir(_output_dir())


func after_each() -> void:
	_cleanup_dir(_output_dir())


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


func _find_multigraphic_particle(particles: Array) -> Dictionary:
	for entry in particles:
		var particle := entry as NovaParticleDef
		if particle == null:
			continue
		var graphics: Array = particle.get_graphics()
		var present_layers := 0
		for layer_entry in graphics:
			var layer := layer_entry as NovaParticleGraphicLayer
			if layer != null and layer.get_present():
				present_layers += 1
		if present_layers > 1:
			return {
				"particle": particle,
				"present_layers": present_layers,
			}
	return {}


func _write_test_texture(path: String) -> void:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color(1.0, 0.35, 0.1, 1.0))
	assert_eq(image.save_png(path), OK, "Test texture should be writable: %s" % path)


func _output_dir() -> String:
	return OS.get_user_data_dir().path_join(OUTPUT_DIR_NAME)


func _cleanup_dir(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	for file in DirAccess.get_files_at(path):
		DirAccess.remove_absolute(path.path_join(file))
	for dir in DirAccess.get_directories_at(path):
		_cleanup_dir(path.path_join(dir))
	DirAccess.remove_absolute(path)


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


func test_particle_viewport_is_shell_managed() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var lane: Control = workstation.get_node("%ViewportLane")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame
	assert_eq(lane.get_child_count(), 1, "Particle workspace should mount exactly one viewport child.")
	assert_eq(str(lane.get_child(0).name), "ParticlePreview")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)
	await get_tree().process_frame
	assert_eq(lane.get_child_count(), 0, "Placeholder workspaces should not inherit the particle preview.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame
	assert_eq(lane.get_child_count(), 1, "Returning to Particles should remount the preview without duplicates.")
	assert_eq(str(lane.get_child(0).name), "ParticlePreview")


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


func test_particle_preview_spawns_visible_instances_after_selection() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation._workspaces[EditorWorkstationScript.Workspace.PARTICLE]
	var err: Error = adapter.open_file(_fixture("buildup.ptl"))
	assert_eq(err, OK, "buildup.ptl should load via the workspace adapter.")
	var lane: Control = workstation.get_node("%ViewportLane")
	var preview := lane.get_child(0) as ParticlePreview
	assert_not_null(preview, "Viewport lane child should be a ParticlePreview.")

	for i in range(12):
		await get_tree().process_frame

	assert_gt(preview.get_alive_count(), 0,
			"Selecting/opening a particle should produce live preview instances.")


func test_particle_preview_populates_render_instances_after_selection() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation._workspaces[EditorWorkstationScript.Workspace.PARTICLE]
	var err: Error = adapter.open_file(_fixture("buildup.ptl"))
	assert_eq(err, OK, "buildup.ptl should load via the workspace adapter.")
	var lane: Control = workstation.get_node("%ViewportLane")
	var preview := lane.get_child(0) as ParticlePreview
	assert_not_null(preview, "Viewport lane child should be a ParticlePreview.")

	for i in range(24):
		await get_tree().process_frame

	assert_gt(preview.get_rendered_instance_count(), 0,
			"Live particles should populate render instances in the preview multimeshes.")


func test_short_lived_particle_preview_loops_after_selection() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation._workspaces[EditorWorkstationScript.Workspace.PARTICLE]
	var err: Error = adapter.open_file(_fixture("30MM.ptl"))
	assert_eq(err, OK, "30MM.ptl should load via the workspace adapter.")
	var flash: NovaParticleDef = adapter.particle_editor.particle_file.find_particle("30mmFlash")
	assert_not_null(flash, "Fixture should include the short-lived 30mmFlash particle.")
	adapter.select_particle(flash)
	var lane: Control = workstation.get_node("%ViewportLane")
	var preview := lane.get_child(0) as ParticlePreview
	assert_not_null(preview, "Viewport lane child should be a ParticlePreview.")

	for i in range(120):
		await get_tree().process_frame

	assert_gt(preview.get_alive_count(), 0,
			"Short-lived particle previews should loop instead of disappearing permanently.")
	assert_gt(preview.get_rendered_instance_count(), 0,
			"Looped short-lived previews should keep render instances populated.")


func test_effect_preview_uses_all_referenced_particle_defs() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation._workspaces[EditorWorkstationScript.Workspace.PARTICLE]
	var err: Error = adapter.open_file(_fixture("30MM.ptl"))
	assert_eq(err, OK, "30MM.ptl should load via the workspace adapter.")
	var effects: Array = adapter.particle_editor.particle_file.get_effects()
	assert_gt(effects.size(), 0, "Fixture should contain at least one effect.")
	var effect := effects[0] as NovaParticleEffect
	workstation._set_workflow(ParticleWorkspaceScript.Workflow.EFFECTS, true)
	await get_tree().process_frame
	adapter.select_effect(effect)
	await get_tree().process_frame

	var lane: Control = workstation.get_node("%ViewportLane")
	assert_eq(lane.get_child_count(), 1, "Particle preview should stay in the shell viewport lane.")
	var preview := lane.get_child(0) as ParticlePreview
	assert_not_null(preview, "Viewport lane child should be a ParticlePreview.")
	assert_eq(preview.get_emitter_count(), effect.pdefs.size(),
			"Effect preview should create one emitter per referenced pdef.")


func test_particle_preview_reports_present_graphic_layers() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation._workspaces[EditorWorkstationScript.Workspace.PARTICLE]
	var err: Error = adapter.open_file(_fixture("30MM.ptl"))
	assert_eq(err, OK, "30MM.ptl should load via the workspace adapter.")
	var selected_info := _find_multigraphic_particle(adapter.particle_editor.particle_file.get_particles())
	var selected := selected_info.get("particle", null) as NovaParticleDef
	var expected_layers := int(selected_info.get("present_layers", 0))

	assert_not_null(selected, "Fixture should include a particle with multiple graphic layers.")
	adapter.select_particle(selected)
	await get_tree().process_frame

	var lane: Control = workstation.get_node("%ViewportLane")
	var preview := lane.get_child(0) as ParticlePreview
	assert_not_null(preview, "Viewport lane child should be a ParticlePreview.")
	assert_eq(preview.get_emitter_count(), 1, "Particle preview should use one emitter for a single pdef.")
	assert_eq(preview.get_visual_layer_count(), expected_layers,
			"Particle preview should expose all present graphic layers.")


func test_particle_preview_routes_particles_to_selected_graphic_layer() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation._workspaces[EditorWorkstationScript.Workspace.PARTICLE]
	var err: Error = adapter.open_file(_fixture("30MM.ptl"))
	assert_eq(err, OK, "30MM.ptl should load via the workspace adapter.")
	var selected_info := _find_multigraphic_particle(adapter.particle_editor.particle_file.get_particles())
	var selected := selected_info.get("particle", null) as NovaParticleDef
	assert_not_null(selected, "Fixture should include a particle with multiple graphic layers.")
	adapter.select_particle(selected)

	var lane: Control = workstation.get_node("%ViewportLane")
	var preview := lane.get_child(0) as ParticlePreview
	assert_not_null(preview, "Viewport lane child should be a ParticlePreview.")
	for i in range(24):
		await get_tree().process_frame

	assert_gt(preview.get_alive_count(), 0, "Selected multi-graphic particle should spawn preview particles.")
	assert_eq(preview.get_rendered_instance_count(), preview.get_alive_count(),
			"Each spawned particle should render through its chosen graphic layer, not every declared layer.")


func test_particle_preview_resolves_graphic_textures_from_particle_file_dir() -> void:
	var texture_dir := _output_dir().path_join("particle_textures")
	assert_eq(DirAccess.make_dir_recursive_absolute(texture_dir), OK)
	_write_test_texture(texture_dir.path_join("line.png"))

	var file := NovaParticleFile.new()
	assert_eq(file.load_from_file(_fixture("buildup.ptl")), OK)
	file.set_source_path(texture_dir.path_join("buildup.ptl"))

	var preview := add_child_autofree(ParticlePreview.new()) as ParticlePreview
	await get_tree().process_frame
	preview.set_particle_file(file)
	preview.set_particle_def(file.find_particle("Buildup dots"))
	await get_tree().process_frame

	var emitter := preview.get_emitter()
	assert_not_null(emitter, "Particle preview should create an emitter for the selected pdef.")
	assert_eq(emitter.get_texture_dir(), texture_dir,
			"Particle emitters should resolve graphics relative to the particle file source directory.")
	assert_eq(emitter.get_resolved_texture_path(0).get_file(), "line.png",
			"Graphic texture lookup should accept alternate texture extensions beside the PTL.")
	assert_eq(preview.get_textured_layer_count(), 1,
			"Resolved particle graphics should bind a texture to the preview layer material.")


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
