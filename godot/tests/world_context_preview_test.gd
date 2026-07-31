extends GutTest

# F1 gate (docs/oned/workspace-maturity-program.md): the WorldContextPreview
# service mounts the shared in-world furniture (clear/environment/sky/weather/water)
# over a caller-provided world root, binds the app-owned environment document,
# and exposes the grounding sampler seam. Drives the service against a plain
# Node3D + stub seam Callables, public API only (ADR 0018).

const WorldContextPreviewScript = preload("res://modtools/framework/world_context_preview.gd")
const EnvironmentEditorScript = preload("res://modtools/environment/environment_editor.gd")

const STUB_WATER_HEIGHT := 12.5
const STUB_GROUND_HEIGHT := 40.0


func _make_preview(world_root: Node3D, calls: Dictionary, material: ShaderMaterial = null):
	return WorldContextPreviewScript.new(
		world_root,
		func() -> ShaderMaterial: return material,
		func() -> float: return STUB_WATER_HEIGHT,
		func(world_x: float, _world_z: float) -> float: return STUB_GROUND_HEIGHT if world_x < 1000.0 else NAN,
		func() -> void: calls["state"] = int(calls.get("state", 0)) + 1
	)


func _mounted_preview(calls: Dictionary, material: ShaderMaterial = null) -> Array:
	var world_root := Node3D.new()
	add_child_autofree(world_root)
	var preview = _make_preview(world_root, calls, material)
	preview.init_environment_preview()
	preview.init_water_plane()
	return [world_root, preview]


func test_init_creates_the_five_furniture_nodes_once_with_exact_names() -> void:
	var pair := _mounted_preview({})
	var world_root: Node3D = pair[0]
	var preview = pair[1]

	assert_eq(world_root.get_child_count(), 5, "clear + env + sky + weather + water land under the world root.")
	assert_not_null(world_root.get_node_or_null("EditorEnvironment"), "The environment node keeps its contract name.")
	assert_not_null(world_root.get_node_or_null("EditorClearColor"), "The frame-clear node keeps its contract name.")
	assert_not_null(world_root.get_node_or_null("EditorSky"), "The sky node keeps its contract name.")
	assert_not_null(world_root.get_node_or_null("EditorWeather"), "The weather node keeps its contract name.")
	assert_not_null(world_root.get_node_or_null("WaterPlane"), "The water node keeps its contract name.")
	assert_eq(preview.get_environment_node(), world_root.get_node("EditorEnvironment"), "Getter hands out the created environment node.")
	assert_true(preview.has_method("get_clear_color_node"), "The preview exposes its frame-clear node.")
	if preview.has_method("get_clear_color_node"):
		assert_eq(preview.get_clear_color_node(), world_root.get_node("EditorClearColor"),
			"Getter hands out the created frame-clear node.")
	assert_eq(preview.get_sky_node(), world_root.get_node("EditorSky"), "Getter hands out the created sky node.")
	assert_eq(preview.get_weather_node(), world_root.get_node("EditorWeather"), "Getter hands out the created weather node.")
	assert_eq(preview.get_water_node(), world_root.get_node("WaterPlane"), "Getter hands out the created water node.")

	assert_eq(preview.get_sky_node().environment_path, NodePath("../EditorEnvironment"), "Sky resolves the environment relatively.")
	assert_eq(preview.get_weather_node().environment_path, NodePath("../EditorEnvironment"), "Weather resolves the environment relatively.")
	assert_eq(preview.get_water_node().environment_path, NodePath("../EditorEnvironment"), "Water resolves the environment relatively.")
	assert_almost_eq(preview.get_water_node().water_height, STUB_WATER_HEIGHT, 0.001,
		"init_water_plane drives the height override through the owner's water-height seam.")

	# Re-init must not duplicate the furniture.
	preview.init_environment_preview()
	preview.init_water_plane()
	assert_eq(world_root.get_child_count(), 5, "re-init keeps the same five nodes.")


func test_preview_sky_tracks_active_camera_with_retail_half_height() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var world_root := Node3D.new()
	viewport.add_child(world_root)
	var first_camera := Camera3D.new()
	first_camera.position = Vector3(10.0, 747.0, 20.0)
	world_root.add_child(first_camera)
	first_camera.current = true

	var preview = _make_preview(world_root, {})
	preview.init_environment_preview()
	var sky: Node3D = preview.get_sky_node()
	await get_tree().process_frame
	await get_tree().process_frame
	assert_eq(sky.mesh_instance.global_position, Vector3(10.0, 373.5, 20.0),
		"ONED keeps the recovered retail half-height dome anchor; the below-rim area is frame clear.")

	var second_camera := Camera3D.new()
	second_camera.position = Vector3(-30.0, 900.0, 45.0)
	world_root.add_child(second_camera)
	second_camera.current = true
	await get_tree().process_frame
	assert_eq(sky.mesh_instance.global_position, Vector3(-30.0, 450.0, 45.0),
		"The preview sky follows the viewport's newly active camera after a workspace camera switch.")


func test_preview_renders_runtime_frame_clear_color_below_the_dome_rim() -> void:
	var pair := _mounted_preview({})
	var preview = pair[1]
	assert_true(preview.has_method("get_clear_color_node"),
		"WorldContextPreview needs the runtime ClearColor consumer for below-rim pixels.")
	if not preview.has_method("get_clear_color_node"):
		return
	var clear := preview.get_clear_color_node() as WorldEnvironment
	assert_not_null(clear, "The editor preview mounts a WorldEnvironment clear-color consumer.")
	if clear == null:
		return
	assert_not_null(clear.environment, "The editor clear node carries an Environment resource.")
	if clear.environment == null:
		return
	assert_eq(clear.environment.background_mode, Environment.BG_COLOR,
		"BG_COLOR renders the witnessed skyfog clear instead of the black null-sky fallback.")
	assert_eq(clear.environment.ambient_light_source, Environment.AMBIENT_SOURCE_DISABLED,
		"The clear color must not inject Godot ambient into OpenNova's authored lighting.")

	var env_editor = add_child_autofree(EnvironmentEditorScript.new())
	preview.bind_environment_editor(env_editor)
	var env_node: Node = preview.get_environment_node()
	var rgb: Vector3 = env_node.get_frame_clear_color()
	assert_eq(clear.environment.background_color, Color(rgb.x, rgb.y, rgb.z),
		"Below-rim ONED pixels use the same frame-clear color as runtime.")

	env_node.set_smoothed_scalars(100.0, env_node.get_sky_height(), 0.0)
	preview.get_sky_node().sync_frame_clear_color()
	rgb = env_node.get_frame_clear_color()
	assert_eq(clear.environment.background_color, Color(rgb.x, rgb.y, rgb.z),
		"The editor clear tracks weather-smoothed frame color every rendered sky frame.")


func test_bind_environment_editor_fans_out_to_the_furniture() -> void:
	var calls := {}
	var pair := _mounted_preview(calls)
	var preview = pair[1]
	var env_editor = add_child_autofree(EnvironmentEditorScript.new())

	preview.bind_environment_editor(env_editor)
	assert_eq(preview.environment_editor, env_editor, "The service holds the bound document.")
	var env_node: Node = preview.get_environment_node()
	assert_eq(env_node.environment_data, env_editor.env_file, "Binding pushes the document's env file into the environment node.")
	assert_almost_eq(env_node.time_of_day, env_editor.time_of_day, 0.001, "Binding pushes the document's preview time.")
	assert_gt(int(calls.get("state", 0)), 0, "Binding fans out through the owner's state-changed seam.")

	# A discrete TOD scrub on the document must reach the preview nodes.
	var before := int(calls.get("state", 0))
	env_editor.set_time_of_day(600.0)
	assert_almost_eq(env_node.time_of_day, 600.0, 0.001, "Document scrubs drive the environment node's time of day.")
	assert_gt(int(calls.get("state", 0)), before, "Document edits keep fanning out through the seam.")


func test_live_environment_generation_reapplies_terrain_uniforms() -> void:
	var material := ShaderMaterial.new()
	material.shader = load("res://shaders/terrain_editor.gdshader")
	var pair := _mounted_preview({}, material)
	var preview = pair[1]
	var env_editor = add_child_autofree(EnvironmentEditorScript.new())
	preview.bind_environment_editor(env_editor)
	var env_node: Node = preview.get_environment_node()

	var stale_fog := Vector3(0.9, 0.1, 0.2)
	material.set_shader_parameter("u_fog_color", stale_fog)
	var generation_before := int(env_node.get_env_generation())
	var live_fog := Vector3(0.125, 0.25, 0.5)
	env_node.set_fog_color_rt(live_fog)
	assert_gt(int(env_node.get_env_generation()), generation_before,
		"Weather-owned render colors move the environment generation.")
	assert_eq(material.get_shader_parameter("u_fog_color"), stale_fog,
		"Precondition: document-event pushes alone leave the terrain material stale.")

	assert_true(preview.has_method("sync_environment_to_preview"),
		"The preview needs one public generation-aware material synchronization seam.")
	if not preview.has_method("sync_environment_to_preview"):
		return
	preview.sync_environment_to_preview()
	assert_eq(material.get_shader_parameter("u_fog_color"), live_fog,
		"A live weather generation must restamp the terrain material before rendering.")

	var scalar_generation := int(env_node.get_env_generation())
	var live_fog_end := float(material.get_shader_parameter("u_fog_end")) + 37.0
	env_node.set_smoothed_scalars(
		live_fog_end, env_node.get_sky_height(), env_node.get_sun_dim_pct())
	assert_gt(int(env_node.get_env_generation()), scalar_generation,
		"Fog-distance smoothing moves the same material-consumed generation.")
	preview.sync_environment_to_preview()
	assert_almost_eq(float(material.get_shader_parameter("u_fog_end")), live_fog_end, 0.001,
		"A scalar-only fog ramp must restamp terrain after the color smoother settles.")


func test_mission_time_override_is_non_mutating_and_restores_authored_time() -> void:
	var pair := _mounted_preview({})
	var preview = pair[1]
	var env_editor = add_child_autofree(EnvironmentEditorScript.new())
	env_editor.set_time_of_day(615.0)
	preview.bind_environment_editor(env_editor)
	var env_node: Node = preview.get_environment_node()

	assert_true(preview.has_method("set_time_of_day_override"),
		"Mission preview needs a non-persistent clock seam separate from ENV authoring.")
	assert_true(preview.has_method("clear_time_of_day_override"),
		"Leaving Mission must restore the ENV document's authored preview time.")
	if not preview.has_method("set_time_of_day_override") \
			or not preview.has_method("clear_time_of_day_override"):
		return

	preview.set_time_of_day_override(800.0)
	assert_almost_eq(env_node.time_of_day, 800.0, 0.001,
		"The Mission view uses the BMS start time instead of the ENV curtime.")
	assert_almost_eq(env_editor.time_of_day, 615.0, 0.001,
		"A Mission preview must not move the Environment workspace's authoring control.")
	assert_eq(env_editor.env_file.get_curtime(), 615,
		"A Mission preview must never mutate the saveable ENV curtime.")

	# Environment edits still update the authored document while the scoped
	# Mission override remains the value rendered in the shared world.
	env_editor.set_time_of_day(930.0)
	assert_almost_eq(env_node.time_of_day, 800.0, 0.001,
		"Document fan-out cannot dislodge an active Mission clock override.")
	assert_eq(env_editor.env_file.get_curtime(), 930)
	preview.clear_time_of_day_override()
	assert_almost_eq(env_node.time_of_day, 930.0, 0.001,
		"Leaving Mission restores the latest authored ENV preview time.")


func test_grounding_helpers_route_through_the_sampler_seam() -> void:
	var pair := _mounted_preview({})
	var preview = pair[1]

	assert_almost_eq(preview.sample_height(10.0, 20.0), STUB_GROUND_HEIGHT, 0.001, "sample_height returns the stubbed surface height.")
	assert_eq(preview.ground_position(Vector3(10.0, 99.0, 20.0)), Vector3(10.0, STUB_GROUND_HEIGHT, 20.0),
		"ground_position snaps the point onto the sampled surface.")
	assert_true(is_nan(preview.sample_height(5000.0, 0.0)), "Off-surface points sample NAN, the owner sampler's contract.")
	assert_eq(preview.ground_position(Vector3(5000.0, 7.0, 3.0)), Vector3(5000.0, 7.0, 3.0),
		"Points the sampler cannot ground come back unchanged.")


func test_release_frees_the_furniture_and_unbinds_the_document() -> void:
	var calls := {}
	var pair := _mounted_preview(calls)
	var world_root: Node3D = pair[0]
	var preview = pair[1]
	var env_editor = add_child_autofree(EnvironmentEditorScript.new())
	preview.bind_environment_editor(env_editor)

	preview.release()
	assert_eq(world_root.get_child_count(), 0, "release() frees the four furniture nodes.")
	assert_null(preview.get_environment_node(), "release() clears the environment node reference.")
	assert_null(preview.get_sky_node(), "release() clears the sky node reference.")
	assert_null(preview.get_weather_node(), "release() clears the weather node reference.")
	assert_null(preview.get_water_node(), "release() clears the water node reference.")
	assert_null(preview.environment_editor, "release() drops the bound document.")

	var after_release := int(calls.get("state", 0))
	env_editor.set_time_of_day(300.0)
	assert_eq(int(calls.get("state", 0)), after_release, "A released service no longer listens to the document.")

	# After release() the service can mount fresh furniture (ViewportMount's
	# release-then-mount contract).
	preview.init_environment_preview()
	preview.init_water_plane()
	assert_eq(world_root.get_child_count(), 5, "init after release rebuilds all five furniture nodes.")
