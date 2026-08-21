extends GutTest

# Particle workspace tests — mirror the terrain workstation tests in shape.
# Verifies the workspace registers in the rail, exposes its three workflows,
# loads a fixture .ptl through the workspace adapter, and propagates dirty
# state when an inspector edits a field.

const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")
const ParticleWorkspaceScript = preload("res://modtools/particle/particle_workspace.gd")
const ParticleEditorScript = preload("res://modtools/particle/particle_editor.gd")
const ParticleEffectInspectorScript = preload("res://modtools/particle/inspectors/effect_inspector.gd")
const ParticleDefInspectorScript = preload("res://modtools/particle/inspectors/particle_inspector.gd")
const ParticleTableInspectorScript = preload("res://modtools/particle/inspectors/table_inspector.gd")
const FlyCameraScript = preload("res://game/fly_camera.gd")

const OUTPUT_DIR_NAME := "particle_editor_workstation_test"
const PARTICLE_FLAG_FOREVER_EMIT := ParticleDef.FLAG_FOREVER_EMIT  # particle_flag::ForeverEmit = 0x40000 (engine flag-table idx 18, post-HAZE)


class DirtyGuardShell:
	extends WorkspaceShell
	var save_action := Callable()
	var discard_action := Callable()
	var status_messages: Array[String] = []

	func prompt_unsaved_for(on_save: Callable, on_discard: Callable, _on_cancel := Callable()) -> void:
		save_action = on_save
		discard_action = on_discard

	func save_then(workspace: EditorWorkspace, on_done: Callable,
			_failure_message := "Save failed.") -> void:
		if workspace.save_current() == OK and on_done.is_valid():
			on_done.call()

	func show_status_message(message: String, _duration := 0.0,
			_severity: StringName = &"info") -> void:
		status_messages.append(message)


class MipmappedTextureProvider:
	extends RefCounted
	var texture: Texture2D

	func _init(value: Texture2D) -> void:
		texture = value

	func load_texture(_name: String) -> Texture2D:
		return texture


const PARTICLE_SHADER_PATHS := [
	"res://shaders/particle/particle_blend_blend.gdshader",
	"res://shaders/particle/particle_blend_additive.gdshader",
	"res://shaders/particle/particle_blend_premult.gdshader",
	"res://shaders/particle/particle_blend_bump.gdshader",
	"res://shaders/particle/particle_blend_mod.gdshader",
	"res://shaders/particle/particle_blend_mod2x.gdshader",
	"res://shaders/particle/particle_blend_bumpadd.gdshader",
	"res://shaders/particle/particle_blend_distort.gdshader",
]


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


# The viewport lane now holds the blueprint screen (graph + preview), so the
# ParticlePreview is nested rather than the lane's direct child.
func _preview_in(root: Node) -> ParticlePreview:
	if root is ParticlePreview:
		return root
	for child in root.get_children():
		var found := _preview_in(child)
		if found != null:
			return found
	return null


func _write_test_texture(path: String) -> void:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color(1.0, 0.35, 0.1, 1.0))
	assert_eq(image.save_png(path), OK, "Test texture should be writable: %s" % path)


func _find_multigraphic_particle(particles: Array) -> Dictionary:
	for entry in particles:
		var particle := entry as ParticleDef
		if particle == null:
			continue
		var graphics: Array = particle.get_graphics()
		var present_layers := 0
		for layer_entry in graphics:
			var layer := layer_entry as ParticleGraphicLayer
			if layer != null and layer.get_present():
				present_layers += 1
		if present_layers > 1:
			return {
				"particle": particle,
				"present_layers": present_layers,
			}
	return {}


func _make_render_test_particle(
		blend_mode: int = 0,
		flip_frames: int = 1,
		flip_rate: int = 8,
		yaw_rot: float = 0.0,
		roll_rot: float = 0.0,
		texture_name: String = "",
		forever_emit: bool = false) -> ParticleDef:
	var particle := ParticleDef.new()
	particle.id = "RenderTestParticle"
	particle.flags = PARTICLE_FLAG_FOREVER_EMIT if forever_emit else 0
	particle.emit_dur = 1.0
	particle.emit_rate = 10.0
	particle.emit_burst = 1
	particle.emit_shape = 0
	particle.age = 5.0
	particle.alpha = 1.0
	particle.scale_value = 4.0
	particle.yaw_rot = yaw_rot
	particle.roll_rot = roll_rot
	particle.color1 = Color(1.0, 1.0, 1.0, 1.0)
	particle.color2 = Color(1.0, 1.0, 1.0, 1.0)
	particle.color3 = Color(1.0, 1.0, 1.0, 1.0)
	particle.color4 = Color(1.0, 1.0, 1.0, 1.0)

	var graphics: Array = particle.get_graphics()
	var layer := graphics[0] as ParticleGraphicLayer
	layer.present = true
	layer.index = 1
	layer.texture = texture_name
	layer.blend_mode = blend_mode
	layer.flip_frames = flip_frames
	layer.flip_rate = flip_rate
	layer.alpha = 1.0
	layer.scale_value = 4.0
	particle.set_graphics(graphics)
	return particle


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
	# The rail is the vertical dock bar; each button labels itself via a nested
	# BarButtonLabel (mirrors terrain_editor_workstation_test).
	var rail: Control = workstation.get_node("%WorkspaceRail")
	var found_label := false
	for child in rail.get_children():
		if child is Button:
			var bar_label := child.find_child("BarButtonLabel", true, false) as Label
			if bar_label != null and bar_label.text == "Particles":
				found_label = true
				break
	assert_true(found_label, "Particles workspace should appear in the rail.")
	var adapter = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE)
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

	var actions_mount: BoxContainer = workstation.get_node("%WorkspaceActionsMount")
	var labels: Array = []
	for child in actions_mount.get_children():
		if child is Button:
			labels.append((child as Button).text)
	# Save As rides the shell's overflow ("More") menu alongside the primary
	# new/open/save buttons; no separate export action.
	assert_eq(labels, ["New PTL", "Open PTL...", "Save PTL", "More"],
			"Particle workspace should expose new/open/save plus the shell overflow menu, without a separate export.")


func test_particle_viewport_is_shell_managed() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var lane: Control = workstation.get_node("%ViewportMount")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame
	assert_eq(lane.get_child_count(), 1, "Particle workspace should mount exactly one viewport child.")
	assert_eq(str(lane.get_child(0).name), "ParticleBlueprint",
			"Particle workspace should mount the blueprint screen (graph + preview).")
	assert_not_null(_preview_in(lane), "The blueprint screen should hold a ParticlePreview.")
	var adapter = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE)
	assert_not_null(adapter.get_viewport_camera(),
			"The workspace should expose the nested preview camera to shared shell controls.")
	assert_eq(workstation.get_editor_camera(), adapter.get_viewport_camera(),
			"The workstation camera seam should resolve to the active particle preview.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)
	await get_tree().process_frame
	assert_eq(lane.get_child_count(), 0, "Placeholder workspaces should not inherit the particle preview.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame
	assert_eq(lane.get_child_count(), 1, "Returning to Particles should remount the blueprint without duplicates.")
	assert_eq(str(lane.get_child(0).name), "ParticleBlueprint")


func test_particle_preview_camera_uses_fly_controls_at_speed_10() -> void:
	var preview := add_child_autofree(ParticlePreview.new()) as ParticlePreview
	await get_tree().process_frame

	var camera := preview.get_preview_camera()
	assert_not_null(camera, "Particle preview should expose its viewport camera.")
	assert_eq(camera.get_script(), FlyCameraScript,
			"Particle preview should use the shared fly/orbit camera controls.")
	assert_true(camera.current, "Particle preview camera should be current in its SubViewport.")
	assert_almost_eq(float(camera.get("fly_speed")), 10.0, 0.001,
			"Particle preview camera should default to fly speed 10.")


func test_open_fixture_loads_effects_and_particles() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE)
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

	var adapter = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE)
	var err: Error = adapter.open_file(_fixture("buildup.ptl"))
	assert_eq(err, OK, "buildup.ptl should load via the workspace adapter.")
	var lane: Control = workstation.get_node("%ViewportMount")
	var preview := _preview_in(lane)
	assert_not_null(preview, "Viewport lane child should be a ParticlePreview.")

	# Selection warm-up already advanced the sim deterministically to the first
	# alive frame (fixed emitter seed + fixed steps). Pause wall-clock
	# auto-advance before yielding: a slow headless runner's clamped 0.1s frame
	# deltas age a finite def out within a couple dozen frames (macos CI hit
	# 0 alive), a fast runner barely advances it — the wall clock is not what
	# these tests measure.
	preview.set_paused(true)
	await get_tree().process_frame

	assert_gt(preview.get_alive_count(), 0,
			"Selecting/opening a particle should produce live preview instances.")


func test_particle_preview_populates_render_instances_after_selection() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE)
	var err: Error = adapter.open_file(_fixture("buildup.ptl"))
	assert_eq(err, OK, "buildup.ptl should load via the workspace adapter.")
	var lane: Control = workstation.get_node("%ViewportMount")
	var preview := _preview_in(lane)
	assert_not_null(preview, "Viewport lane child should be a ParticlePreview.")

	preview.set_paused(true)
	await get_tree().process_frame

	assert_gt(preview.get_rendered_instance_count(), 0,
			"Live particles should populate render instances in the preview particle batches.")


func test_particle_preview_uses_value_emitters_and_one_shared_draw_list_renderer() -> void:
	var preview := add_child_autofree(ParticlePreview.new()) as ParticlePreview
	await get_tree().process_frame
	preview.set_particle_def(_make_render_test_particle())
	preview.set_paused(true)

	var emitter := preview.get_emitter() as Dictionary
	assert_false(emitter.is_empty(), "Selected particle should expose value diagnostics.")
	assert_true(emitter.has("emitter_id"), "Diagnostics should expose the stable emitter id.")
	assert_true(emitter.has("frame"), "Diagnostics should include the frame value snapshot.")
	assert_true(emitter.has("render"), "Diagnostics should include joined draw list bounds.")
	for descendant in preview.find_children("*", "", true, false):
		assert_ne(descendant.get_class(), "NovaParticleEmitter",
				"ONED preview must not create a render node per particle emitter.")
	assert_gt(preview.get_render_batch_count(), 0,
			"Shared renderer should expose its world draw-list diagnostics.")


func test_particle_preview_renders_mipmapped_provider_texture() -> void:
	# Mounted retail textures are normalized with mipmaps before the shared
	# renderer reads them. The atlas boundary must consume mip 0 without treating
	# the lower levels as extra pixels and rejecting an otherwise valid texture.
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color(1.0, 0.35, 0.1, 1.0))
	image.generate_mipmaps()
	assert_true(image.has_mipmaps(), "precondition: mounted texture is mipmapped")
	var texture := ImageTexture.create_from_image(image)
	assert_true(texture.get_image().has_mipmaps(),
			"precondition: provider texture preserves its mip levels")
	var provider := MipmappedTextureProvider.new(texture)
	var preview := add_child_autofree(ParticlePreview.new()) as ParticlePreview
	await get_tree().process_frame
	preview.set_texture_provider(Callable(provider, "load_texture"))
	preview.set_particle_def(_make_render_test_particle(
			0, 1, 8, 0.0, 0.0, "mounted_particle.tga"))
	preview.set_paused(true)
	await get_tree().process_frame

	assert_gt(preview.get_alive_count(), 0,
			"Mipmapped provider texture should retain a live preview particle.")
	assert_gt(preview.get_rendered_instance_count(), 0,
			"Mipmapped provider texture should produce a visible draw list quad.")
	assert_gt(preview.get_render_batch_count(), 0,
			"Mipmapped provider texture should produce a shared renderer batch.")


func test_particle_shaders_keep_depth_tests_and_scene_fog() -> void:
	for path in PARTICLE_SHADER_PATHS:
		var shader := load(path) as Shader
		assert_not_null(shader, "Particle shader should load: %s" % path)
		if shader == null:
			continue
		var code := String(shader.code)
		assert_true(code.contains("depth_draw_never"),
				"Particles should remain transparent and never write depth: %s" % path)
		assert_false(code.contains("depth_test_disabled"),
				"World geometry should occlude particles: %s" % path)
		assert_false(code.contains("fog_disabled"),
				"Scene fog should affect particles: %s" % path)


func test_additive_particle_shaders_do_not_double_apply_alpha() -> void:
	var additive := load(PARTICLE_SHADER_PATHS[1]) as Shader
	var bumpadd := load(PARTICLE_SHADER_PATHS[6]) as Shader
	assert_not_null(additive)
	assert_not_null(bumpadd)
	if additive != null:
		var additive_code := String(additive.code)
		# Witnessed additive = ONE/INVSRCALPHA with the type-1 atlas alpha clear:
		# the fragment alpha is a constant 0 (pure add) and the authored alpha
		# curve never attenuates an additive layer
		# [orig: CParticleTexture_InitTextureAndChannels @ 0x5e8380;
		#  BuildTextureAtlases alpha clear @ 0x5e9116].
		assert_true(additive_code.contains("blend_premul_alpha"),
				"Additive must use the witnessed ONE/INVSRCALPHA pair, not blend_add.")
		assert_true(additive_code.contains("ALPHA = 0.0;"),
				"Additive fragments carry the cleared type-1 page alpha (pure add).")
		assert_false(additive_code.contains("* base.a")
				or additive_code.contains("* COLOR.a"),
				"Additive color must not be attenuated by any alpha source.")
	if bumpadd != null:
		var bumpadd_code := String(bumpadd.code)
		assert_true(bumpadd_code.contains("ALBEDO = vec3(0.0);"),
				"Bumpadd keeps the natively lit base channel black.")
		assert_true(bumpadd_code.contains("EMISSION = vec3(dotv) * 1.25;"),
				"Bumpadd presents its DOT3 result as restrained HDR emission.")
		assert_false(bumpadd_code.contains("dotv * texel.a")
				or bumpadd_code.contains("dotv * COLOR.a"),
				"Bumpadd color must not be premultiplied a second time.")


func test_premult_particle_shader_keeps_modulate_semantics() -> void:
	# Witnessed premult = the same MODULATE(TEXTURE, DIFFUSE) program as blend,
	# under ONE/INVSRCALPHA — DIFFUSE alpha lands in the fragment alpha (the
	# destination attenuation) and retail never scales the source RGB by it
	# [orig: CParticleTexture_InitTextureAndChannels @ 0x5e8380 + @ 0x5e85a9].
	var premult := load(PARTICLE_SHADER_PATHS[2]) as Shader
	assert_not_null(premult)
	if premult == null:
		return
	var code := String(premult.code)
	assert_true(code.contains("blend_premul_alpha"),
			"Premult must keep the witnessed ONE/INVSRCALPHA pair.")
	assert_true(code.contains("ALBEDO = base.rgb;"),
			"Premult RGB is the plain texture-by-DIFFUSE modulate; no extra opacity factor.")
	assert_false(code.contains("ALBEDO = base.rgb * opacity"),
			"Retail premult does not rescale source RGB by particle alpha.")


func test_timeline_scrub_pauses_and_holds_the_requested_frame() -> void:
	var preview := add_child_autofree(ParticlePreview.new()) as ParticlePreview
	await get_tree().process_frame
	preview.set_particle_def(_make_render_test_particle(0, 1, 8, 0.0, 0.0, "", true))
	preview._on_timeline_changed(10.0)
	assert_true(preview.is_paused(), "a user scrub takes manual control of playback")
	assert_eq(preview.get_current_frame(), 10)
	await get_tree().process_frame
	await get_tree().process_frame
	assert_eq(preview.get_current_frame(), 10,
			"auto-advance stays off after the deterministic seek")


func test_short_lived_particle_preview_does_not_auto_repeat_after_selection() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE)
	var err: Error = adapter.open_file(_fixture("30MM.ptl"))
	assert_eq(err, OK, "30MM.ptl should load via the workspace adapter.")
	var flash: ParticleDef = adapter.particle_editor.particle_file.find_particle("30mmFlash")
	assert_not_null(flash, "Fixture should include the short-lived 30mmFlash particle.")
	adapter.select_particle(flash)
	var lane: Control = workstation.get_node("%ViewportMount")
	var preview := _preview_in(lane)
	assert_not_null(preview, "Viewport lane child should be a ParticlePreview.")

	var emitter := preview.get_emitter() as Dictionary
	assert_false(emitter.is_empty(), "Short-lived preview should have one value emitter.")
	assert_true(emitter.has("emitter_id"), "Emitter diagnostics should have a stable id.")
	# Let the preview's own wall-clock playback run the one-shot to completion:
	# a fixed frame count under-ages on a fast headless runner and over-waits on
	# a slow one, so pace on the scene-backed preview's finished flag (bounded).
	for i in range(600):
		if preview.is_finished():
			break
		await get_tree().process_frame
	assert_true(preview.is_finite(), "Short-lived fixture particle should be finite.")
	assert_true(preview.is_finished(), "Finite one-shot preview should finish instead of auto-repeating.")
	assert_eq(preview.get_alive_count(), 0,
			"Short-lived finite particle previews should not auto-repeat after all particles expire.")
	assert_eq(preview.get_rendered_instance_count(), 0,
			"Finished one-shot previews should clear rendered particle batches.")

	preview.restart()
	# restart() un-pauses and warm-ups deterministically to the first alive
	# frame; pause before yielding so the awaited frame's wall-clock delta
	# (clamped 0.1s on a loaded runner) cannot age the flash out again — the
	# same race the selection-path tests pause against.
	preview.set_paused(true)
	await get_tree().process_frame
	assert_gt(preview.get_alive_count(), 0,
			"Manual preview restart should still replay a completed one-shot particle.")
	assert_false(preview.is_finished(),
			"Manual preview restart should reset the one-shot scene completion state.")


func test_effect_preview_uses_all_referenced_particle_defs() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE)
	var err: Error = adapter.open_file(_fixture("30MM.ptl"))
	assert_eq(err, OK, "30MM.ptl should load via the workspace adapter.")
	var effects: Array = adapter.particle_editor.particle_file.get_effects()
	assert_gt(effects.size(), 0, "Fixture should contain at least one effect.")
	var effect := effects[0] as ParticleEffect
	workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE).select_workflow(ParticleWorkspaceScript.Workflow.EFFECTS)
	await get_tree().process_frame
	adapter.select_effect(effect)
	await get_tree().process_frame

	var lane: Control = workstation.get_node("%ViewportMount")
	assert_eq(lane.get_child_count(), 1, "Particle preview should stay in the shell viewport lane.")
	var preview := _preview_in(lane)
	assert_not_null(preview, "Viewport lane child should be a ParticlePreview.")
	assert_eq(preview.get_emitter_count(), effect.pdefs.size(),
			"Effect preview should create one value emitter per referenced pdef.")


func test_particle_preview_reports_present_graphic_layers() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE)
	var err: Error = adapter.open_file(_fixture("30MM.ptl"))
	assert_eq(err, OK, "30MM.ptl should load via the workspace adapter.")
	var selected_info := _find_multigraphic_particle(adapter.particle_editor.particle_file.get_particles())
	var selected := selected_info.get("particle", null) as ParticleDef
	var expected_layers := int(selected_info.get("present_layers", 0))

	assert_not_null(selected, "Fixture should include a particle with multiple graphic layers.")
	adapter.select_particle(selected)
	await get_tree().process_frame

	var lane: Control = workstation.get_node("%ViewportMount")
	var preview := _preview_in(lane)
	assert_not_null(preview, "Viewport lane child should be a ParticlePreview.")
	assert_eq(preview.get_emitter_count(), 1,
			"Particle preview should use one value emitter for a single pdef.")
	assert_eq(preview.get_visual_layer_count(), expected_layers,
			"Particle preview should expose all present graphic layers.")


func test_particle_preview_layer_stats_read_the_retained_editor_model() -> void:
	var particle := _make_render_test_particle()
	var preview := add_child_autofree(ParticlePreview.new()) as ParticlePreview
	await get_tree().process_frame
	preview.set_particle_def(particle)
	preview.set_paused(true)
	await get_tree().process_frame
	assert_eq(preview.get_visual_layer_count(), 1)

	# Stats refreshes run continuously. They should read the retained editor
	# values directly instead of serializing the scene's immutable catalog and
	# every live particle merely to count four graphic slots.
	var graphics: Array = particle.get_graphics()
	var second := graphics[1] as ParticleGraphicLayer
	second.present = true
	second.index = 2
	second.texture = "second_layer.tga"
	particle.set_graphics(graphics)
	assert_eq(preview.get_visual_layer_count(), 2,
			"Layer stats should observe the edited model without a scene rebuild.")


func test_particle_preview_routes_particles_to_selected_graphic_layer() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE)
	var err: Error = adapter.open_file(_fixture("30MM.ptl"))
	assert_eq(err, OK, "30MM.ptl should load via the workspace adapter.")
	var selected_info := _find_multigraphic_particle(adapter.particle_editor.particle_file.get_particles())
	var selected := selected_info.get("particle", null) as ParticleDef
	assert_not_null(selected, "Fixture should include a particle with multiple graphic layers.")
	adapter.select_particle(selected)

	var lane: Control = workstation.get_node("%ViewportMount")
	var preview := _preview_in(lane)
	assert_not_null(preview, "Viewport lane child should be a ParticlePreview.")
	preview.set_paused(true)
	await get_tree().process_frame

	assert_gt(preview.get_alive_count(), 0, "Selected multi-graphic particle should spawn preview particles.")
	assert_eq(preview.get_rendered_instance_count(), preview.get_alive_count(),
			"Each spawned particle should render through its chosen graphic layer, not every declared layer.")


func test_particle_preview_resolves_graphic_textures_from_particle_file_dir() -> void:
	var texture_dir := _output_dir().path_join("particle_textures")
	assert_eq(DirAccess.make_dir_recursive_absolute(texture_dir), OK)
	_write_test_texture(texture_dir.path_join("line.png"))

	var file := ParticleFile.new()
	assert_eq(file.load_from_file(_fixture("buildup.ptl")), OK)
	file.set_source_path(texture_dir.path_join("buildup.ptl"))

	var preview := add_child_autofree(ParticlePreview.new()) as ParticlePreview
	await get_tree().process_frame
	preview.set_particle_file(file)
	preview.set_particle_def(file.find_particle("Buildup dots"))
	await get_tree().process_frame

	var emitter := preview.get_emitter() as Dictionary
	assert_false(emitter.is_empty(), "Particle preview should expose the selected pdef emitter.")
	assert_eq(preview.get_texture_dir(), texture_dir,
			"Shared renderer should resolve graphics relative to the particle file source directory.")
	assert_false(preview.get_unresolved_texture_names().has("line.tga"),
			"Graphic texture lookup should accept the line.png alternate extension beside the PTL.")
	assert_eq(preview.get_textured_layer_count(), 1,
			"Resolved particle graphics should occupy a textured shared-renderer layer.")


func test_workflow_swap_mounts_correct_inspector() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE)
	adapter.open_file(_fixture("30MM.ptl"))
	var inspector_mount: Control = workstation.get_node("%InspectorMount")

	# Default workflow is PARTICLES.
	workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE).select_workflow(ParticleWorkspaceScript.Workflow.PARTICLES)
	await get_tree().process_frame
	assert_not_null(_find_inspector_of_type(inspector_mount, ParticleDefInspectorScript),
			"Particles workflow should mount the particle inspector.")

	workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE).select_workflow(ParticleWorkspaceScript.Workflow.EFFECTS)
	await get_tree().process_frame
	assert_not_null(_find_inspector_of_type(inspector_mount, ParticleEffectInspectorScript),
			"Effects workflow should mount the effect inspector.")

	workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE).select_workflow(ParticleWorkspaceScript.Workflow.TABLES)
	await get_tree().process_frame
	assert_not_null(_find_inspector_of_type(inspector_mount, ParticleTableInspectorScript),
			"Tables workflow should mount the table inspector.")


func test_invalid_particle_save_reports_failure_and_writes_nothing() -> void:
	var workspace := ParticleWorkspaceScript.new()
	var particle = workspace.particle_editor.add_particle()
	particle.id = ""
	var dir := _output_dir().path_join("blocked_save")
	DirAccess.make_dir_recursive_absolute(dir)
	assert_eq(workspace.save_as(dir), ERR_INVALID_DATA,
			"validation blocks must not masquerade as successful saves")
	assert_false(FileAccess.file_exists(dir.path_join("untitled.ptl")),
			"an invalid document is not written")
	assert_true(workspace.get_save_failure_message(ERR_INVALID_DATA).contains("empty name"),
			"the shell can preserve the specific validation reason")


func test_particle_save_as_uses_the_chosen_file_name() -> void:
	var workspace := ParticleWorkspaceScript.new()
	workspace.particle_editor.add_particle()
	var dir := _output_dir().path_join("named_save")
	DirAccess.make_dir_recursive_absolute(dir)
	var path := dir.path_join("custom-name.ptl")

	assert_true(workspace.uses_save_file_dialog(), "A PTL is a single named file, not a project directory.")
	assert_true(workspace.get_save_file_dialog_filters().has("*.ptl,*.PTL ; NovaLogic Particle"))
	assert_eq(workspace.get_save_file_dialog_default_name(), "untitled.ptl")
	assert_eq(workspace.save_as_file(path), OK)
	assert_true(FileAccess.file_exists(path), "Save As writes the exact file selected by the user.")
	assert_eq(workspace.particle_editor.current_path, path)
	assert_eq(workspace.get_save_file_dialog_default_name(), "custom-name.ptl")


func test_dirty_new_and_open_wait_for_discard_or_successful_save() -> void:
	var workspace := ParticleWorkspaceScript.new()
	var shell := DirtyGuardShell.new()
	add_child_autofree(shell)
	workspace.set_editor_shell(shell)
	workspace.particle_editor.add_particle()
	assert_true(workspace.particle_editor.is_dirty)

	assert_eq(workspace.new_current(), OK)
	assert_eq(workspace.particle_editor.particle_count(), 1,
			"New PTL does not replace the dirty document before confirmation")
	assert_true(shell.discard_action.is_valid())
	shell.discard_action.call()
	assert_eq(workspace.particle_editor.particle_count(), 0,
			"Discard runs the deferred New continuation")

	workspace.particle_editor.add_particle()
	shell.discard_action = Callable()
	assert_eq(workspace.open_file(_fixture("buildup.ptl")), OK)
	assert_true(workspace.particle_editor.current_path.is_empty(),
			"Open PTL does not replace the dirty document before confirmation")
	assert_true(shell.discard_action.is_valid())
	shell.discard_action.call()
	assert_eq(workspace.particle_editor.current_path, _fixture("buildup.ptl"))
	assert_false(workspace.particle_editor.is_dirty)


func test_failed_deferred_open_reports_error_and_keeps_dirty_document() -> void:
	var workspace := ParticleWorkspaceScript.new()
	var shell := DirtyGuardShell.new()
	add_child_autofree(shell)
	workspace.set_editor_shell(shell)
	workspace.particle_editor.add_particle()
	var corrupt_path := _output_dir().path_join("corrupt.ptl")
	DirAccess.make_dir_recursive_absolute(_output_dir())
	var corrupt := FileAccess.open(corrupt_path, FileAccess.WRITE)
	assert_not_null(corrupt)
	corrupt.store_string("[particledef]\n{\nid = Broken;\n")
	corrupt.close()

	assert_eq(workspace.open_file(corrupt_path), OK,
			"the dirty guard owns the asynchronous open result")
	assert_true(shell.discard_action.is_valid())
	shell.discard_action.call()
	assert_true(workspace.particle_editor.current_path.is_empty(),
			"a failed deferred open does not replace the dirty document")
	assert_true(workspace.particle_editor.is_dirty)
	assert_false(shell.status_messages.is_empty(), "the deferred failure is surfaced through the shell")
	assert_true(shell.status_messages[-1].contains("Open failed"))


func test_property_edit_marks_document_dirty() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.PARTICLE)
	await get_tree().process_frame

	var adapter = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.PARTICLE)
	adapter.open_file(_fixture("buildup.ptl"))
	assert_false(adapter.particle_editor.is_dirty, "Fresh-loaded document is clean.")

	# Touch a field on the selected particle directly + mark dirty (mirrors what
	# the inspector does on a SpinBox value_changed). After that, save action
	# should be enabled.
	var particle: ParticleDef = adapter.particle_editor.current_particle
	assert_not_null(particle, "Open should pre-select the first particle.")
	particle.set_emit_dur(particle.get_emit_dur() + 1.0)
	adapter.particle_editor.mark_dirty()
	assert_true(adapter.particle_editor.is_dirty, "Edits should mark the document dirty.")
	assert_true(adapter.can_save(), "Dirty + path => save action available.")
