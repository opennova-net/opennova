extends GutTest


class FakePresentation:
	extends RefCounted

	var trace: Array[String]
	var seen_input: MissionFrameInput
	var playing := true

	func _init(p_trace: Array[String]) -> void:
		trace = p_trace

	func is_playing() -> bool:
		return playing

	func advance_session_frame(input: MissionFrameInput) -> MissionFrameOutcome:
		seen_input = input
		trace.append("session")
		return MissionFrameOutcome.new()


class FakeWorld:
	extends Node

	var trace: Array[String] = []
	var network_ok := true
	var presentation := FakePresentation.new(trace)
	var camera_generation := 0
	var terrain_camera_generation := -1
	var foliage_camera_generation := -1
	var framefx_camera_generation := -1

	func begin_device_frame(_camera_pos: Vector3, _camera_xform: Transform3D,
			_delta: float) -> void:
		trace.append("begin")

	func render_terrain_frame() -> void:
		terrain_camera_generation = camera_generation
		trace.append("terrain")
	func render_foliage_frame() -> void:
		foliage_camera_generation = camera_generation
		trace.append("foliage")
	func get_active_frame_stats_board() -> FrameStatsBoard: return null
	func is_device_frame_timing_enabled() -> bool: return false
	func record_runtime_frame(_elapsed_us: int) -> void: pass
	func get_runtime() -> FakePresentation: return presentation
	func present_local_view_frame() -> void:
		camera_generation += 1
		trace.append("local_view")
	func sync_framefx_frame() -> void:
		framefx_camera_generation = camera_generation
		trace.append("framefx")
	func apply_scene_environment_frame() -> void:
		trace.append("scene_environment")
	func render_environment_nodes_frame() -> void:
		trace.append("environment_nodes")
	func render_water_frame() -> void:
		trace.append("water")
	func drive_network_frame() -> bool:
		trace.append("network")
		return network_ok
	func advance_weather_frame() -> void: trace.append("weather")
	func apply_blink_frame() -> void: trace.append("blink")
	func apply_occlusion_frame() -> void: trace.append("occlusion")
	func sample_iris_frame() -> void: trace.append("iris")
	func render_sun_veil_frame() -> void: trace.append("sun_veil")
	func render_light_frame() -> void: trace.append("lights")
	func render_slot_shadow_frame() -> void: trace.append("slot_shadows")
	func render_material_frame() -> void: trace.append("materials")
	func render_particle_frame() -> void: trace.append("particles")
	func mix_audio_frame(ticks_run: int) -> void:
		trace.append("audio:%d" % ticks_run)
	func update_clear_frame() -> void: trace.append("clear")
	func render_environment_cube_frame() -> void: trace.append("environment_cube")
	func finish_device_frame() -> void: trace.append("finish")
	func session_frame_failed(_reason: String) -> void: trace.append("failed")


func test_pipeline_orders_one_typed_session_call_between_concrete_devices() -> void:
	var world := FakeWorld.new()
	add_child_autofree(world)
	var pipeline := GameFramePipeline.new()
	pipeline.setup(world)
	var input := MissionFrameInput.new()

	var outcome := pipeline.advance(Vector3(1, 2, 3), Transform3D.IDENTITY,
			0.0125, input)

	assert_not_null(outcome)
	assert_same(world.presentation.seen_input, input,
			"the one sampled input object crosses the pipeline unchanged")
	assert_almost_eq(input.delta_seconds, 0.0125, 0.000001)
	assert_eq(world.trace, [
		"begin", "session", "local_view", "framefx", "scene_environment",
		"environment_nodes", "water", "terrain", "foliage", "network",
		"weather", "occlusion", "iris", "sun_veil", "lights", "materials", "slot_shadows",
		"particles", "audio:0", "clear", "environment_cube", "finish",
	])
	assert_eq(world.terrain_camera_generation, 1,
			"terrain samples the post-present camera generation")
	assert_eq(world.foliage_camera_generation, 1,
			"foliage samples the post-present camera generation")
	# The renderer's auxiliary views restore their color into the beauty
	# target, so a pose one generation stale shears every surface the beauty
	# pass still owns (foliage, scars, coronas) whenever the view turns.
	assert_eq(world.framefx_camera_generation, 1,
			"the auxiliary render views sample the post-present camera generation")


func test_network_install_failure_suppresses_every_later_device_phase() -> void:
	var world := FakeWorld.new()
	add_child_autofree(world)
	world.network_ok = false
	var pipeline := GameFramePipeline.new()
	pipeline.setup(world)

	pipeline.advance(Vector3.ZERO, Transform3D.IDENTITY, 0.016,
			MissionFrameInput.new())

	assert_eq(world.trace,
			["begin", "session", "local_view", "framefx", "scene_environment",
			"environment_nodes", "water", "terrain", "foliage", "network"])
