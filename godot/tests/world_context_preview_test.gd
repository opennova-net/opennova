extends GutTest

# F1 gate (docs/oned/workspace-maturity-program.md): the WorldContextPreview
# service mounts the shared in-world furniture (environment/sky/weather/water)
# over a caller-provided world root, binds the app-owned environment document,
# and exposes the grounding sampler seam. Drives the service against a plain
# Node3D + stub seam Callables, public API only (ADR 0018).

const WorldContextPreviewScript = preload("res://modtools/framework/world_context_preview.gd")
const EnvironmentEditorScript = preload("res://modtools/environment/environment_editor.gd")

const STUB_WATER_HEIGHT := 12.5
const STUB_GROUND_HEIGHT := 40.0


func _make_preview(world_root: Node3D, calls: Dictionary):
	return WorldContextPreviewScript.new(
		world_root,
		func() -> ShaderMaterial: return null,
		func() -> float: return STUB_WATER_HEIGHT,
		func(world_x: float, _world_z: float) -> float: return STUB_GROUND_HEIGHT if world_x < 1000.0 else NAN,
		func() -> void: calls["state"] = int(calls.get("state", 0)) + 1
	)


func _mounted_preview(calls: Dictionary) -> Array:
	var world_root := Node3D.new()
	add_child_autofree(world_root)
	var preview = _make_preview(world_root, calls)
	preview.init_environment_preview()
	preview.init_water_plane()
	return [world_root, preview]


func test_init_creates_the_four_furniture_nodes_once_with_exact_names() -> void:
	var pair := _mounted_preview({})
	var world_root: Node3D = pair[0]
	var preview = pair[1]

	assert_eq(world_root.get_child_count(), 4, "env + sky + weather + water land under the world root.")
	assert_not_null(world_root.get_node_or_null("EditorEnvironment"), "The environment node keeps its contract name.")
	assert_not_null(world_root.get_node_or_null("EditorSky"), "The sky node keeps its contract name.")
	assert_not_null(world_root.get_node_or_null("EditorWeather"), "The weather node keeps its contract name.")
	assert_not_null(world_root.get_node_or_null("WaterPlane"), "The water node keeps its contract name.")
	assert_eq(preview.get_environment_node(), world_root.get_node("EditorEnvironment"), "Getter hands out the created environment node.")
	assert_eq(preview.get_sky_node(), world_root.get_node("EditorSky"), "Getter hands out the created sky node.")
	assert_eq(preview.get_weather_node(), world_root.get_node("EditorWeather"), "Getter hands out the created weather node.")
	assert_eq(preview.get_water_node(), world_root.get_node("WaterPlane"), "Getter hands out the created water node.")

	assert_eq(preview.get_sky_node().environment_path, NodePath("../EditorEnvironment"), "Sky resolves the environment relatively.")
	assert_eq(preview.get_weather_node().environment_path, NodePath("../EditorEnvironment"), "Weather resolves the environment relatively.")
	assert_eq(preview.get_water_node().environment_path, NodePath("../EditorEnvironment"), "Water resolves the environment relatively.")
	assert_almost_eq(preview.get_water_node().water_height, STUB_WATER_HEIGHT, 0.001,
		"init_water_plane drives the height override through the host's water-height seam.")

	# Re-init must not duplicate the furniture.
	preview.init_environment_preview()
	preview.init_water_plane()
	assert_eq(world_root.get_child_count(), 4, "re-init keeps the same four nodes.")


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
	assert_gt(int(calls.get("state", 0)), 0, "Binding fans out through the host's state-changed seam.")

	# A discrete TOD scrub on the document must reach the preview nodes.
	var before := int(calls.get("state", 0))
	env_editor.set_time_of_day(600.0)
	assert_almost_eq(env_node.time_of_day, 600.0, 0.001, "Document scrubs drive the environment node's time of day.")
	assert_gt(int(calls.get("state", 0)), before, "Document edits keep fanning out through the seam.")


func test_grounding_helpers_route_through_the_sampler_seam() -> void:
	var pair := _mounted_preview({})
	var preview = pair[1]

	assert_almost_eq(preview.sample_height(10.0, 20.0), STUB_GROUND_HEIGHT, 0.001, "sample_height returns the stubbed surface height.")
	assert_eq(preview.ground_position(Vector3(10.0, 99.0, 20.0)), Vector3(10.0, STUB_GROUND_HEIGHT, 20.0),
		"ground_position snaps the point onto the sampled surface.")
	assert_true(is_nan(preview.sample_height(5000.0, 0.0)), "Off-surface points sample NAN, the host sampler's contract.")
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
	assert_eq(world_root.get_child_count(), 4, "init after release rebuilds the furniture.")
