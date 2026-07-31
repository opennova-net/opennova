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
const FlyCameraScript = preload("res://engine/fly_camera.gd")

const OUTPUT_DIR_NAME := "particle_editor_workstation_test"
const PARTICLE_FLAG_FOREVER_EMIT := NovaParticleDef.FLAG_FOREVER_EMIT  # particle_flag::ForeverEmit = 0x40000 (engine flag-table idx 18, post-HAZE)


class DirtyGuardShell:
	extends Node
	var save_action := Callable()
	var discard_action := Callable()
	var status_messages: Array[String] = []

	func prompt_unsaved_for(on_save: Callable, on_discard: Callable) -> void:
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


func _make_render_test_particle(
		blend_mode: int = 0,
		flip_frames: int = 1,
		flip_rate: int = 8,
		yaw_rot: float = 0.0,
		roll_rot: float = 0.0,
		texture_name: String = "",
		forever_emit: bool = false) -> NovaParticleDef:
	var particle := NovaParticleDef.new()
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
	var layer := graphics[0] as NovaParticleGraphicLayer
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


func _add_synthetic_render_emitter() -> NovaParticleEmitter:
	var emitter := NovaParticleEmitter.new()
	# Synthetic render probes deliberately have no texture. Runtime emitters
	# keep blank authored graphics invisible; these editor diagnostics opt in.
	emitter.procedural_fallback_enabled = true
	add_child_autofree(emitter)
	return emitter


func _add_render_test_emitter(particle: NovaParticleDef, dt: float = 0.26) -> NovaParticleEmitter:
	var emitter := _add_synthetic_render_emitter()
	emitter.auto_advance = false
	emitter.def = particle
	emitter.play()
	emitter.advance(dt)
	return emitter


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


func test_particle_preview_uses_value_emitters_and_one_shared_packet_renderer() -> void:
	var preview := add_child_autofree(ParticlePreview.new()) as ParticlePreview
	await get_tree().process_frame
	preview.set_particle_def(_make_render_test_particle())
	preview.set_paused(true)

	var emitter := preview.get_emitter() as Dictionary
	assert_false(emitter.is_empty(), "Selected particle should expose value diagnostics.")
	assert_true(emitter.has("emitter_id"), "Diagnostics should expose the stable emitter id.")
	assert_true(emitter.has("frame"), "Diagnostics should include the frame value snapshot.")
	assert_true(emitter.has("render"), "Diagnostics should include joined packet bounds.")
	for descendant in preview.find_children("*", "", true, false):
		assert_false(descendant is NovaParticleEmitter,
				"ONED preview must not create a render node per particle emitter.")
	assert_gt(preview.get_render_batch_count(), 0,
			"Shared renderer should expose its world draw-packet diagnostics.")


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
			"Mipmapped provider texture should produce a visible packet quad.")
	assert_gt(preview.get_render_batch_count(), 0,
			"Mipmapped provider texture should produce a shared renderer batch.")


func test_particle_renderer_builds_rotated_quads() -> void:
	# Initial roll seeds from def.orientation.z (+ orientationadj.z * rand01),
	# in degrees [orig: CParticleEmitter_SpawnParticle @ 0x5e7803 — def+3824
	# into particle+0x3C]; yaw_rot/roll_rot are angular RATES, not seeds.
	var particle := _make_render_test_particle(0, 1, 8)
	particle.orientation = Vector3(0.0, 0.0, 45.0)
	var emitter := _add_render_test_emitter(particle)

	assert_gt(emitter.get_rendered_instance_count(), 0,
			"Synthetic particle should render at least one quad.")
	assert_almost_eq(emitter.get_debug_first_rotation(), deg_to_rad(45.0), 0.01,
			"Renderer should seed the billboard roll from orientation.z.")
	var quad: PackedVector3Array = emitter.get_debug_first_quad_vertices()
	assert_eq(quad.size(), 4, "Renderer should expose the first debug quad vertices.")
	var top_edge := quad[1] - quad[0]
	assert_gt(absf(top_edge.y), 0.001,
			"Rotated billboard quad should not remain axis-aligned in the preview plane.")


func test_particle_renderer_advances_flipbook_uv_frame() -> void:
	# Flipbook clock = 4 x flip_rate x elapsed seconds [orig:
	# BuildBillboardQuads @ 0x5e6f17 — (256/phase_rate) * flip_rate * phase / 64]:
	# rate 8, dt 0.07 -> int(4 * 8 * 0.07) = 2 of 4 frames.
	var emitter := _add_render_test_emitter(_make_render_test_particle(0, 4, 8), 0.07)

	assert_gt(emitter.get_rendered_instance_count(), 0,
			"Synthetic flipbook particle should render at least one quad.")
	assert_eq(emitter.get_debug_first_flip_frame(), 2,
			"flip_frames/flip_rate should advance the rendered UV frame at 4x flip_rate per second.")


func test_particle_renderer_keeps_blend_mode_and_depth_counts() -> void:
	var emitter := _add_render_test_emitter(_make_render_test_particle(1))

	assert_eq(emitter.get_debug_first_blend_mode(), 1,
			"Renderer should preserve the parsed additive blend mode on its render batch.")
	assert_eq(emitter.get_sorted_depth_count(), emitter.get_alive_count(),
			"Depth sorting should account for every live rendered particle.")
	assert_gt(emitter.get_render_batch_count(), 0,
			"Live particles should produce at least one render batch.")


# CParticleEmitter_RenderStaticBillboards @ 0x5f4e10 — selected when
# def.flags & 0x100 (= particle_flag::YawAndPitch, bit 8) is set. The static
# path renders WORLD-ORIENTED quads through the per-particle (yaw, pitch,
# roll) Euler matrix [orig: @ 0x5f5068] — not rotation-suppressed billboards.
const PARTICLE_FLAG_YAW_AND_PITCH := NovaParticleDef.FLAG_YAW_AND_PITCH


func test_yaw_and_pitch_renders_world_oriented_quads() -> void:
	# pitch seeds from orientation.y; 90 degrees tips the quad out of the
	# camera plane (headless default right/up = world X/Y), so the corner Z
	# coordinates must diverge — impossible on the billboard path.
	var particle := _make_render_test_particle(0, 1, 8)
	particle.flags = PARTICLE_FLAG_YAW_AND_PITCH
	particle.orientation = Vector3(0.0, 90.0, 0.0)
	var emitter := _add_render_test_emitter(particle)

	assert_gt(emitter.get_rendered_instance_count(), 0,
			"YAWANDPITCH particle should still render at least one quad.")
	assert_true(emitter.get_debug_static_billboard(),
			"YAWANDPITCH bit should drive the renderer onto the world-oriented path.")
	var quad: PackedVector3Array = emitter.get_debug_first_quad_vertices()
	assert_eq(quad.size(), 4, "World-oriented path should expose the debug quad.")
	var z_span := 0.0
	for v in quad:
		z_span = maxf(z_span, absf(v.z - quad[0].z))
	assert_gt(z_span, 0.001,
			"90-degree pitch must tip the world-oriented quad out of the XY plane.")


func test_yaw_and_pitch_clear_keeps_rotation() -> void:
	# roll_rot is an angular RATE (deg/s, random sign unless SIGNEDROTATIONS)
	# [orig: SpawnParticle @ 0x5e782a]; after 0.26 s a 90 deg/s roll shows up
	# in the accumulated billboard rotation.
	var emitter := _add_render_test_emitter(_make_render_test_particle(0, 1, 8, 0.0, 90.0))

	assert_false(emitter.get_debug_static_billboard(),
			"Without YAWANDPITCH the renderer should take the rotated billboard path.")
	assert_gt(absf(emitter.get_debug_first_rotation()), 0.001,
			"Rotated billboard path must apply the accumulated per-particle roll.")


# CParticleDefEntry_ParseBlendMode @ 0x5e29f0 → 8 distinct shader paths under
# shaders/particle/ (engine-layer, shipped in the runtime export). The renderer
# caches one ShaderMaterial per layer and assigns the matching shader resource
# for the parsed BlendMode.
func test_blend_mode_dispatches_matching_shader() -> void:
	# Additive (1) → particle_blend_additive.gdshader
	var add_emitter := _add_render_test_emitter(_make_render_test_particle(1))
	assert_eq(add_emitter.get_debug_first_blend_mode(), 1)
	var add_path := add_emitter.get_debug_first_shader_path()
	assert_true(add_path.ends_with("particle_blend_additive.gdshader"),
			"Additive blend mode should bind particle_blend_additive.gdshader (got %s)" % add_path)

	# Blend (0) → particle_blend_blend.gdshader
	var blend_emitter := _add_render_test_emitter(_make_render_test_particle(0))
	assert_eq(blend_emitter.get_debug_first_blend_mode(), 0)
	var blend_path := blend_emitter.get_debug_first_shader_path()
	assert_true(blend_path.ends_with("particle_blend_blend.gdshader"),
			"Blend mode should bind particle_blend_blend.gdshader (got %s)" % blend_path)

	# Mod (4) → particle_blend_mod.gdshader
	var mod_emitter := _add_render_test_emitter(_make_render_test_particle(4))
	assert_eq(mod_emitter.get_debug_first_blend_mode(), 4)
	var mod_path := mod_emitter.get_debug_first_shader_path()
	assert_true(mod_path.ends_with("particle_blend_mod.gdshader"),
			"Mod blend mode should bind particle_blend_mod.gdshader (got %s)" % mod_path)


func test_blend_mode_distort_routes_to_distort_shader() -> void:
	var emitter := _add_render_test_emitter(_make_render_test_particle(7))
	assert_eq(emitter.get_debug_first_blend_mode(), 7)
	var path := emitter.get_debug_first_shader_path()
	assert_true(path.ends_with("particle_blend_distort.gdshader"),
			"Distort blend mode should bind particle_blend_distort.gdshader (got %s)" % path)


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
		assert_true(bumpadd_code.contains("ALBEDO = vec3(dotv);"),
				"Bumpadd should pass its DOT3 result straight to the additive blend state.")
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


func test_particle_shader_resources_drive_all_render_batches() -> void:
	for blend_mode in range(PARTICLE_SHADER_PATHS.size()):
		var emitter := _add_synthetic_render_emitter()
		emitter.auto_advance = false
		emitter.def = _make_render_test_particle(blend_mode)
		emitter.play()
		emitter.advance(0.26)
		assert_gt(emitter.get_rendered_instance_count(), 0,
				"Blend mode %d should produce a rendered particle batch." % blend_mode)
		var material := emitter.get_debug_layer_material(0) as ShaderMaterial
		assert_not_null(material, "Blend mode %d should bind a ShaderMaterial." % blend_mode)
		if material == null:
			continue
		assert_not_null(material.shader, "Blend mode %d should load its shader." % blend_mode)
		if material.shader != null:
			assert_eq(String(material.shader.resource_path), PARTICLE_SHADER_PATHS[blend_mode])


# CParticleEmitter_BuildBillboardQuads @ 0x5e6d60: per-channel `(emitter_byte
# * channel) >> 7` modulates the rendered color by emitter+200..202. We
# expose this as `color_tint` (Color, default white = neutral). The renderer
# applies it after curve modulation, before the 0..1 clamp.
func test_color_tint_default_is_neutral() -> void:
	var emitter := _add_render_test_emitter(_make_render_test_particle(0))
	var tint: Color = emitter.color_tint
	assert_almost_eq(tint.r, 1.0, 0.001, "default tint.r is 1.0 (neutral)")
	assert_almost_eq(tint.g, 1.0, 0.001, "default tint.g is 1.0 (neutral)")
	assert_almost_eq(tint.b, 1.0, 0.001, "default tint.b is 1.0 (neutral)")
	# White-particle * neutral-tint should leave color near (1,1,1).
	var rendered: Color = emitter.get_debug_first_color()
	assert_almost_eq(rendered.r, 1.0, 0.05, "neutral tint preserves r")
	assert_almost_eq(rendered.g, 1.0, 0.05, "neutral tint preserves g")
	assert_almost_eq(rendered.b, 1.0, 0.05, "neutral tint preserves b")


func test_color_tint_zeroes_channel() -> void:
	var emitter := _add_synthetic_render_emitter()
	emitter.auto_advance = false
	emitter.color_tint = Color(0.0, 1.0, 1.0, 1.0)  # zero out red
	emitter.def = _make_render_test_particle(0)
	emitter.play()
	emitter.advance(0.26)

	assert_gt(emitter.get_rendered_instance_count(), 0)
	var rendered: Color = emitter.get_debug_first_color()
	assert_lt(rendered.r, 0.01, "tint.r=0 zeroes the rendered red channel (got %f)" % rendered.r)
	assert_gt(rendered.g, 0.5, "tint.g=1 preserves green channel (got %f)" % rendered.g)
	assert_gt(rendered.b, 0.5, "tint.b=1 preserves blue channel (got %f)" % rendered.b)


const PARTICLE_FLAG_POSITION_RELATIVE := NovaParticleDef.FLAG_POSITION_RELATIVE


# CParticleEmitter_BuildBillboardQuads @ 0x5e6d60: lit-color path triggered
# when particle.flags & 0x80 (LitColor for Bump=3 / Bumpadd=6 blend modes).
# Retail negates the raw constants' first two components, then encodes
# `bump_scale x M^T x (+1/sqrt(3), +1/sqrt(3), +1/sqrt(3))` by truncating
# `(value + 1) x 0.5 x 255` and retaining the low byte without saturation.
func test_lit_color_default_neutral_when_not_bump() -> void:
	# blend mode 0 (Blend) → particle.flags has no LitColor bit → lit_color
	# stays neutral white in our renderer.
	var emitter := _add_render_test_emitter(_make_render_test_particle(0))
	var lit: Color = emitter.get_debug_first_lit_color()
	assert_almost_eq(lit.r, 1.0, 0.001, "non-bump lit_color.r is neutral 1.0")
	assert_almost_eq(lit.g, 1.0, 0.001, "non-bump lit_color.g is neutral 1.0")
	assert_almost_eq(lit.b, 1.0, 0.001, "non-bump lit_color.b is neutral 1.0")


func test_lit_color_encoded_when_bump_blend_mode() -> void:
	# blend mode 3 (Bump) → particle.flags has LitColor bit set during spawn.
	# With default bump_scale = 0, the per-axis term zeroes regardless of
	# rotation: encoded value = (0 + 1) × 0.5 = 0.5 → byte 127 ≈ 0.498 in
	# [0, 1] (RGBA8 quantisation).
	var emitter := _add_render_test_emitter(_make_render_test_particle(3))
	var lit: Color = emitter.get_debug_first_lit_color()
	assert_almost_eq(lit.r, 0.5, 0.02, "bump lit_color.r ≈ 0.5 (default bump_scale=0)")
	assert_almost_eq(lit.g, 0.5, 0.02, "bump lit_color.g ≈ 0.5")
	assert_almost_eq(lit.b, 0.5, 0.02, "bump lit_color.b ≈ 0.5")


func test_lit_color_channels_match_retail_seed_at_zero_roll() -> void:
	# Identity view + zero roll leaves the witnessed (+k,+k,+k) seed equal in
	# all channels: trunc((1+k)*127.5) = 201.
	var particle := _make_render_test_particle(3)
	particle.bump_scale = 1.0
	var emitter := _add_render_test_emitter(particle)
	var lit: Color = emitter.get_debug_first_lit_color()
	var expected := 201.0 / 255.0
	assert_almost_eq(lit.r, expected, 0.01, "retail +k seed packs red byte 201")
	assert_almost_eq(lit.g, expected, 0.01, "retail +k seed packs green byte 201")
	assert_almost_eq(lit.b, expected, 0.01, "retail +k seed packs blue byte 201")


func test_lit_color_retains_low_byte_without_saturation() -> void:
	# bump_scale=3 yields trunc((1+3k)*127.5)=348; retail keeps byte 92
	# instead of saturating to 255.
	var particle := _make_render_test_particle(3)
	particle.bump_scale = 3.0
	var emitter := _add_render_test_emitter(particle)
	var lit: Color = emitter.get_debug_first_lit_color()
	var expected := 92.0 / 255.0
	assert_almost_eq(lit.r, expected, 0.01, "red retains low byte 92")
	assert_almost_eq(lit.g, expected, 0.01, "green retains low byte 92")
	assert_almost_eq(lit.b, expected, 0.01, "blue retains low byte 92")


func test_lit_color_varies_with_particle_rotation() -> void:
	# Two emitters with the same particle but different roll_rot values
	# should produce different lit_colors when bump_scale > 0. This drives
	# the rotation-aware path: rp.rotation feeds local_right/local_up
	# which feed the dot products with the engine light direction.
	var particle_a := _make_render_test_particle(3, 1, 8, 0.0, 0.0)
	particle_a.bump_scale = 1.0
	var emitter_a := _add_render_test_emitter(particle_a)
	var lit_a: Color = emitter_a.get_debug_first_lit_color()

	var particle_b := _make_render_test_particle(3, 1, 8, 0.0, 90.0)  # 90° roll
	particle_b.bump_scale = 1.0
	var emitter_b := _add_render_test_emitter(particle_b)
	var lit_b: Color = emitter_b.get_debug_first_lit_color()

	# At least one channel should differ meaningfully between rotations.
	var dr: float = absf(lit_a.r - lit_b.r)
	var dg: float = absf(lit_a.g - lit_b.g)
	var db: float = absf(lit_a.b - lit_b.b)
	var max_delta: float = maxf(dr, maxf(dg, db))
	assert_gt(max_delta, 0.1,
			"lit_color should differ between rotation 0° and 90° (got max delta %f)" % max_delta)


func test_atlas_texture_combines_multiple_layers() -> void:
	# CParticleManager_BuildTextureAtlases @ 0x5e8db0: per-emitter atlas
	# combining all present graphic layers' textures into one image, with
	# atlas-relative UV rects. The wrapper adds a 1-pixel gutter around each
	# packed layer to prevent linear-filter bleed between loose textures.
	# Unique texture dir per test: the engine's texture_path_resolver caches each
	# directory's listing, so two tests writing different textures into one shared
	# dir would have the second miss the first's stale cache. A per-test dir gives
	# each its own cache key and a fresh enumeration.
	var dir := _output_dir().path_join("atlas_combine")
	DirAccess.make_dir_recursive_absolute(dir)
	var path0 := dir.path_join("atlas_layer_a.png")
	var path1 := dir.path_join("atlas_layer_b.png")
	# Distinct sizes pin the horizontal-shelf layout: layer 0 = 8w, layer 1 = 16w.
	var image0 := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image0.fill(Color(1.0, 0.0, 0.0, 1.0))
	assert_eq(image0.save_png(path0), OK, "layer 0 source PNG written")
	var image1 := Image.create(16, 8, false, Image.FORMAT_RGBA8)
	image1.fill(Color(0.0, 1.0, 0.0, 1.0))
	assert_eq(image1.save_png(path1), OK, "layer 1 source PNG written")

	var particle := _make_render_test_particle(0, 1, 8, 0.0, 0.0, "atlas_layer_a.png")
	var graphics: Array = particle.get_graphics()
	var layer1 := graphics[1] as NovaParticleGraphicLayer
	layer1.present = true
	layer1.index = 2
	layer1.texture = "atlas_layer_b.png"
	layer1.blend_mode = 0
	layer1.flip_frames = 1
	layer1.flip_rate = 8
	layer1.alpha = 1.0
	layer1.scale_value = 4.0
	particle.set_graphics(graphics)

	var emitter := _add_synthetic_render_emitter()
	emitter.auto_advance = false
	emitter.texture_dir = dir
	emitter.def = particle
	emitter.play()
	emitter.advance(0.26)

	var atlas: ImageTexture = emitter.get_debug_atlas_texture()
	assert_not_null(atlas, "Atlas texture should be built when both layers loaded.")
	assert_eq(atlas.get_width(), 28, "Atlas width includes 1px gutters around both layers.")
	assert_eq(atlas.get_height(), 10, "Atlas height includes top/bottom gutters.")

	var atlas_image := atlas.get_image()
	assert_not_null(atlas_image, "Atlas image should be readable for gutter verification.")
	assert_eq(atlas_image.get_pixel(0, 1), Color(1.0, 0.0, 0.0, 1.0),
			"Layer 0 left gutter duplicates the red edge pixel.")
	assert_eq(atlas_image.get_pixel(9, 1), Color(1.0, 0.0, 0.0, 1.0),
			"Layer 0 right gutter duplicates the red edge pixel.")
	assert_eq(atlas_image.get_pixel(10, 1), Color(0.0, 1.0, 0.0, 1.0),
			"Layer 1 left gutter duplicates the green edge pixel.")
	assert_eq(atlas_image.get_pixel(27, 1), Color(0.0, 1.0, 0.0, 1.0),
			"Layer 1 right gutter duplicates the green edge pixel.")

	# Every layer material's albedo_tex points at the same atlas RID.
	var atlas_rid := atlas.get_rid()
	for i in range(4):
		var material: ShaderMaterial = emitter.get_debug_layer_material(i)
		assert_not_null(material, "Layer %d should have a material." % i)
		var bound: Texture2D = material.get_shader_parameter("albedo_tex")
		assert_not_null(bound, "Layer %d should have albedo_tex bound." % i)
		assert_eq(bound.get_rid(), atlas_rid,
				"Layer %d albedo_tex points at the shared atlas." % i)


func test_atlas_clears_when_def_unset() -> void:
	# Setting def back to null releases the atlas reference + resets the
	# rebuild signature so a subsequent set_def rebuilds.
	# Per-test texture dir (own resolver-cache key); see test_atlas_texture_combines.
	var dir := _output_dir().path_join("atlas_clear")
	DirAccess.make_dir_recursive_absolute(dir)
	var path0 := dir.path_join("atlas_clear_a.png")
	_write_test_texture(path0)
	var particle := _make_render_test_particle(0, 1, 8, 0.0, 0.0, "atlas_clear_a.png")
	var emitter := _add_synthetic_render_emitter()
	emitter.auto_advance = false
	emitter.texture_dir = dir
	emitter.def = particle
	emitter.play()
	emitter.advance(0.26)
	assert_not_null(emitter.get_debug_atlas_texture(), "Atlas built when def has present layers.")

	emitter.def = null
	assert_null(emitter.get_debug_atlas_texture(),
			"Atlas should clear when def is unset.")


func test_world_space_default_keeps_particles_when_emitter_moves() -> void:
	# Engine default (PositionRelative clear): particles render in world
	# space. Translating the emitter mid-life does NOT carry alive particles
	# along. Quad world-coordinates remain near the spawn position.
	var emitter := _add_synthetic_render_emitter()
	emitter.auto_advance = false
	emitter.def = _make_render_test_particle(0)
	emitter.play()
	emitter.advance(0.26)

	var quad_before: PackedVector3Array = emitter.get_debug_first_quad_vertices()
	assert_eq(quad_before.size(), 4, "quad available pre-translate")
	var center_before: Vector3 = (quad_before[0] + quad_before[3]) * 0.5

	emitter.global_position = Vector3(50.0, 0.0, 0.0)
	await get_tree().process_frame
	# No advance — alive particles untouched. Re-render to refresh quad data.
	emitter.advance(0.0)
	var quad_after: PackedVector3Array = emitter.get_debug_first_quad_vertices()
	var center_after: Vector3 = (quad_after[0] + quad_after[3]) * 0.5

	# Particle stays in world space (within ~0.5 unit of spawn since the
	# particle's velocity was ~zero with shape=Point).
	assert_lt(center_after.distance_to(center_before), 1.0,
			"world-space default leaves particle near its spawn world position (got %s vs %s)" % [center_after, center_before])
	assert_gt(center_after.distance_to(emitter.global_position), 40.0,
			"world-space default does NOT carry the particle to the new emitter position")


func test_position_relative_carries_particles_with_emitter() -> void:
	# PositionRelative flag (bit 19 = 0x80000): alive particles translate
	# with the emitter when its position changes. Quad world-coordinates
	# shift by the same delta as the emitter.
	var particle := _make_render_test_particle(0)
	particle.flags = PARTICLE_FLAG_POSITION_RELATIVE
	var emitter := _add_synthetic_render_emitter()
	emitter.auto_advance = false
	emitter.def = particle
	emitter.play()
	emitter.advance(0.26)

	var quad_before: PackedVector3Array = emitter.get_debug_first_quad_vertices()
	var center_before: Vector3 = (quad_before[0] + quad_before[3]) * 0.5

	emitter.global_position = Vector3(50.0, 0.0, 0.0)
	await get_tree().process_frame
	emitter.advance(0.0)
	var quad_after: PackedVector3Array = emitter.get_debug_first_quad_vertices()
	var center_after: Vector3 = (quad_after[0] + quad_after[3]) * 0.5
	var observed_delta: Vector3 = center_after - center_before

	assert_almost_eq(observed_delta.x, 50.0, 1.0,
			"PositionRelative carries the alive particle by the emitter delta on x")
	assert_almost_eq(observed_delta.y, 0.0, 1.0,
			"PositionRelative leaves y unchanged when emitter only moves on x")


func test_lod_divisor_default_renders_every_particle() -> void:
	# CParticleEmitter_BuildBillboardQuads @ 0x5e6d60: lod_divisor=1 is the
	# engine default at full perf budget (1.0/budget rounds to 1). Every
	# alive particle reaches the render batch.
	var particle := _make_render_test_particle(0)
	particle.emit_rate = 20.0
	particle.emit_dur = 0.5
	var emitter := _add_synthetic_render_emitter()
	emitter.auto_advance = false
	emitter.def = particle
	assert_eq(emitter.lod_divisor, 1, "default lod_divisor is 1")
	emitter.play()
	emitter.advance(0.4)
	assert_eq(emitter.get_rendered_instance_count(), emitter.get_alive_count(),
			"lod_divisor=1 renders every alive particle")


func test_lod_divisor_skips_particles_by_serial() -> void:
	# lod_divisor=4 → renderer keeps only particles with serial % 4 == 0.
	# Spawn enough particles that the divisor produces a measurable drop.
	# Engine cite: BuildBillboardQuads pre-loop counts visible particles
	# via `if (!(serial % v82)) ++visible_count`.
	var particle := _make_render_test_particle(0)
	particle.emit_rate = 20.0
	particle.emit_dur = 0.5
	var emitter := _add_synthetic_render_emitter()
	emitter.auto_advance = false
	emitter.def = particle
	emitter.lod_divisor = 4
	emitter.play()
	emitter.advance(0.4)

	var alive := emitter.get_alive_count()
	var rendered := emitter.get_rendered_instance_count()
	# With divisor=4, expect roughly alive/4 rendered (within ±1 from
	# rounding boundaries since serials may not span a clean modulo class).
	assert_gt(alive, 4, "spawned enough alive particles for divisor=4 to matter")
	assert_lt(rendered, alive,
			"lod_divisor=4 reduces rendered count below alive count")
	var expected_min: int = (alive / 4) - 1
	var expected_max: int = (alive / 4) + 2
	assert_between(rendered, expected_min, expected_max,
			"rendered count ≈ alive/4 for divisor=4 (got %d, alive=%d)" % [rendered, alive])


func test_kill_plane_mode_property_clamps_invalid_values() -> void:
	# Engine: bits 27/28 in def.flags select kill-above vs kill-at/below.
	# Our portable scalar mode accepts {0=Disabled, 1=KillAbove,
	# 2=KillAtOrBelow}; out-of-range values clamp to Disabled.
	var emitter := _add_synthetic_render_emitter()
	assert_eq(emitter.kill_plane_mode, 0, "default kill_plane_mode is 0 (Disabled)")

	emitter.kill_plane_mode = 1
	assert_eq(emitter.kill_plane_mode, 1, "kill_plane_mode = 1 (KillAbove) accepted")

	emitter.kill_plane_mode = 2
	assert_eq(emitter.kill_plane_mode, 2, "kill_plane_mode = 2 (KillAtOrBelow) accepted")

	emitter.kill_plane_mode = 5
	assert_eq(emitter.kill_plane_mode, 0, "out-of-range clamps to Disabled")

	emitter.kill_plane_mode = -1
	assert_eq(emitter.kill_plane_mode, 0, "negative clamps to Disabled")

	emitter.kill_plane_y = -3.5
	assert_almost_eq(emitter.kill_plane_y, -3.5, 0.001,
			"kill_plane_y is direct float passthrough (no clamp)")


func test_lod_divisor_setter_clamps_to_one() -> void:
	# Setter clamps non-positive values to 1 (matches engine post-round
	# guard where divisor = max(1, round(1.0 / budget))).
	var emitter := _add_synthetic_render_emitter()
	emitter.lod_divisor = 0
	assert_eq(emitter.lod_divisor, 1, "setter clamps 0 to 1")
	emitter.lod_divisor = -3
	assert_eq(emitter.lod_divisor, 1, "setter clamps negative to 1")
	emitter.lod_divisor = 8
	assert_eq(emitter.lod_divisor, 8, "valid divisor passes through")


func test_single_emitter_aabb_center_tracks_world_position() -> void:
	# CParticleManager_TransformToViewSpace @ 0x5ecc50 projects each
	# emitter's bbox to view space; Godot's transparent renderer does the
	# equivalent automatically using the MeshInstance3D's world-space AABB
	# center. With the world-space rendering refactor (top_level=true on
	# layer meshes + vertex data in world coords), the AABB center should
	# track the emitter's world position.
	var emitter := _add_synthetic_render_emitter()
	emitter.auto_advance = false
	emitter.def = _make_render_test_particle(0)
	emitter.global_position = Vector3(7.0, 0.0, 0.0)
	emitter.play()
	emitter.advance(0.26)

	var center: Vector3 = emitter.get_debug_first_layer_aabb_center()
	assert_almost_eq(center.x, 7.0, 5.0,
			"layer mesh AABB center tracks emitter world x")


func test_cross_emitter_aabb_centers_at_distinct_world_positions() -> void:
	# Two emitters at distinct world positions produce mesh AABBs centered
	# near each emitter — Godot's transparent renderer uses these centers
	# as the cross-emitter sort key (back-to-front by view-space depth),
	# matching CParticleManager_RecursiveSortAndRender @ 0x5ec980.
	var em_a := _add_synthetic_render_emitter()
	em_a.auto_advance = false
	em_a.def = _make_render_test_particle(0)
	em_a.global_position = Vector3(-10.0, 0.0, 0.0)
	em_a.play()
	em_a.advance(0.26)

	var em_b := _add_synthetic_render_emitter()
	em_b.auto_advance = false
	em_b.def = _make_render_test_particle(0)
	em_b.global_position = Vector3(10.0, 0.0, 0.0)
	em_b.play()
	em_b.advance(0.26)

	var center_a: Vector3 = em_a.get_debug_first_layer_aabb_center()
	var center_b: Vector3 = em_b.get_debug_first_layer_aabb_center()

	assert_almost_eq(center_a.x, -10.0, 5.0,
			"emitter A's mesh AABB center near world x = -10 (got %f)" % center_a.x)
	assert_almost_eq(center_b.x, 10.0, 5.0,
			"emitter B's mesh AABB center near world x = +10 (got %f)" % center_b.x)
	assert_gt(center_b.x - center_a.x, 15.0,
			"AABB centers separate by ~20 world units → distinct sort keys for Godot's transparent renderer")


func test_emitter_position_change_translates_simulator() -> void:
	# CParticleEmitter_TranslatePosition @ 0x5efe90: when the emitter Node3D
	# is moved, the simulator's last_translation_delta should reflect the
	# per-frame delta. set_notify_transform(true) is enabled in
	# NOTIFICATION_READY so the engine forwards every transform change.
	var emitter := _add_synthetic_render_emitter()
	emitter.auto_advance = false
	emitter.def = _make_render_test_particle(0)
	emitter.play()
	# After init at the default origin, no translate has happened yet.
	var initial: Vector3 = emitter.get_debug_last_translation_delta()
	assert_almost_eq(initial.length(), 0.0, 0.001,
			"freshly-initialised emitter has zero translation delta")

	emitter.global_position = Vector3(5.0, 0.0, 0.0)
	await get_tree().process_frame
	var first: Vector3 = emitter.get_debug_last_translation_delta()
	assert_almost_eq(first.x, 5.0, 0.001, "first move applies +x delta")
	assert_almost_eq(first.y, 0.0, 0.001, "first move y delta = 0")
	assert_almost_eq(first.z, 0.0, 0.001, "first move z delta = 0")

	emitter.global_position = Vector3(5.0, 3.0, -1.0)
	await get_tree().process_frame
	var second: Vector3 = emitter.get_debug_last_translation_delta()
	assert_almost_eq(second.x, 0.0, 0.001, "second move x delta = 0 (no x change)")
	assert_almost_eq(second.y, 3.0, 0.001, "second move applies +y delta")
	assert_almost_eq(second.z, -1.0, 0.001, "second move applies -z delta")


func test_color_tint_boost_clamps_at_one() -> void:
	# Engine clamps each channel at 255 after the (byte * channel) >> 7
	# multiply. Our portable form clamps at 1.0 — a 2× boost on a fully
	# saturated channel still saturates at 1.0.
	var emitter := _add_synthetic_render_emitter()
	emitter.auto_advance = false
	emitter.color_tint = Color(2.0, 2.0, 2.0, 1.0)
	emitter.def = _make_render_test_particle(0)
	emitter.play()
	emitter.advance(0.26)

	var rendered: Color = emitter.get_debug_first_color()
	assert_almost_eq(rendered.r, 1.0, 0.001, "2x tint on r=1 clamps at 1.0")
	assert_almost_eq(rendered.g, 1.0, 0.001, "2x tint on g=1 clamps at 1.0")
	assert_almost_eq(rendered.b, 1.0, 0.001, "2x tint on b=1 clamps at 1.0")


func test_forever_emit_particle_stays_unfinished_after_emit_duration() -> void:
	var particle := _make_render_test_particle(0, 1, 8, 0.0, 0.0, "", true)
	var emitter := _add_render_test_emitter(particle)
	emitter.advance(2.0)

	assert_false(emitter.is_finite(), "FOREVEREMIT particles should report non-finite emission.")
	assert_false(emitter.is_finished(), "FOREVEREMIT particles should remain eligible for continuous preview.")
	assert_gt(emitter.get_alive_count(), 0,
			"FOREVEREMIT particles should continue spawning after finite emit_dur would have elapsed.")


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
	var flash: NovaParticleDef = adapter.particle_editor.particle_file.find_particle("30mmFlash")
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
	var effect := effects[0] as NovaParticleEffect
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
	var selected := selected_info.get("particle", null) as NovaParticleDef
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
	var second := graphics[1] as NovaParticleGraphicLayer
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
	var selected := selected_info.get("particle", null) as NovaParticleDef
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

	var file := NovaParticleFile.new()
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
	var particle: NovaParticleDef = adapter.particle_editor.current_particle
	assert_not_null(particle, "Open should pre-select the first particle.")
	particle.set_emit_dur(particle.get_emit_dur() + 1.0)
	adapter.particle_editor.mark_dirty()
	assert_true(adapter.particle_editor.is_dirty, "Edits should mark the document dirty.")
	assert_true(adapter.can_save(), "Dirty + path => save action available.")
