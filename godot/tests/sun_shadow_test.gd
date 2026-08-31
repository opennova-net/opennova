extends GutTest


func _environment_at(time_of_day: int, node_name: String) -> MissionEnvironment:
	var data := EnvFile.new()
	data.reset_to_default()
	data.set_curtime(time_of_day)
	var environment := MissionEnvironment.new()
	environment.name = node_name
	environment.environment_data = data
	add_child_autofree(environment)
	return environment


func _expected_emission(environment: MissionEnvironment) -> Vector3:
	# get_light_direction already serves the Godot-axes surface->light vector
	# (the env_axes x/z swap of the raw getter tuple IS the (g2, g1, g0)
	# reduction); the shadow projection then clamps the vertical component to
	# 0.25 and negates — renderer::sun_shadow_direction, which SunShadow
	# applies (ADR 0043 keeps this one law from the retired slot system)
	# [orig: Environment_GetLightDirectionFloat @ 0x57d870;
	#  render_shadow_pass @ 0x5d7b70].
	var g := environment.get_light_direction()
	return -Vector3(g.x, maxf(g.y, 0.25), g.z).normalized()


func _assert_tracks_environment(
		light: SunShadow, environment: MissionEnvironment, context: String) -> void:
	var expected_ray := _expected_emission(environment)
	var actual_ray := -light.global_basis.z.normalized()
	assert_true(actual_ray.is_equal_approx(expected_ray), context)


func test_ready_applies_preassigned_environment_direction_synchronously() -> void:
	var environment := _environment_at(1200, "ReadyEnvironment")
	var light := SunShadow.new()
	light.set_environment_node(environment)
	add_child_autofree(light)

	assert_true(light.visible, "a loaded environment enables the sun during ready")
	_assert_tracks_environment(light, environment,
			"ready publishes the first sun direction before a process frame")


func test_environment_assignment_reorients_a_warm_sun_synchronously() -> void:
	var day_environment := _environment_at(1200, "DayEnvironment")
	var night_environment := _environment_at(2200, "NightEnvironment")
	var light := SunShadow.new()
	light.set_environment_node(day_environment)
	add_child_autofree(light)
	# Establish the old direction explicitly so this test isolates the warm
	# environment rebind rather than depending on ready's contract.
	light.advance_frame(0.0)
	var day_ray := -light.global_basis.z.normalized()
	var night_ray := -night_environment.get_light_direction().normalized()
	assert_false(day_ray.is_equal_approx(night_ray),
			"the fixture must use distinct day and night light directions")

	light.set_environment_node(night_environment)

	_assert_tracks_environment(light, night_environment,
			"a warm environment rebind takes effect before another process frame")


func test_the_sun_is_a_real_casting_light() -> void:
	# ADR 0043: one scene sun — real color at MODULATE2X energy, one CSM every
	# lit receiver takes, caster/cull masks wide open (per-instance cast
	# settings gate casting).
	var environment := _environment_at(1200, "CastingEnvironment")
	var light := SunShadow.new()
	light.set_environment_node(environment)
	add_child_autofree(light)
	light.advance_frame(0.0)

	assert_true(light.shadow_enabled, "the sun casts the scene's one CSM")
	assert_eq(light.directional_shadow_mode,
			DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS)
	assert_almost_eq(light.light_energy, 2.0, 0.001,
			"MODULATE2X carried as light energy")
	assert_almost_eq(light.light_specular, 0.0, 0.001,
			"fixed-function materials had no sun specular")
	assert_eq(light.light_cull_mask, 0xFFFFFFFF,
			"every lit receiver takes the sun")
	assert_eq(light.shadow_caster_mask, 0xFFFFFFFF,
			"per-instance cast settings gate casting, not the mask")
	var values: EnvLightValues = environment.get_light_state().get_values()
	# The witnessed dir_color bytes set RAW: Godot's canonical Light3D color
	# decode carries them to the linear scene (ADR 0043 linear-scene
	# amendment; the gamma-contract pre-encode is retired).
	var d := values.get_dir_color()
	assert_true(light.light_color.is_equal_approx(Color(d.x, d.y, d.z)),
			"the sun carries the env dir color raw for the canonical decode")


func test_game_world_composes_one_live_sun_during_ready() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	var data := EnvFile.new()
	data.reset_to_default()
	data.set_curtime(1500)
	var environment := world.get_node("MissionEnvironment") as MissionEnvironment
	environment.environment_data = data
	add_child_autofree(world)

	var live_sun := world.get_node("SunShadow") as SunShadow
	_assert_tracks_environment(live_sun, world.get_environment_node(),
			"GameWorld's sun is aligned when production composition returns")
	assert_false(world.has_node("SlotShadow"),
		"the render-slot capture device is retired (ADR 0043)")


func test_low_sun_projection_clamps_the_vertical_component() -> void:
	# 06:30 sunrise: the getter tuple's vertical is ~0.1227, well under the
	# witnessed 0.25 projection clamp, so shadows project as if the sun sat at
	# ~14.5 deg — silhouettes never stretch past 4x height [orig:
	# render_shadow_pass @ 0x5d7b70 clamp; same constant as the static
	# collector @ 0x60d33f..0x60d341].
	var environment := _environment_at(630, "SunriseEnvironment")
	var light := SunShadow.new()
	light.set_environment_node(environment)
	add_child_autofree(light)

	var g := environment.get_light_direction()
	assert_lt(g.y, 0.25, "the sunrise fixture must sit under the clamp")
	var emission := -light.global_basis.z.normalized()
	assert_true(emission.is_equal_approx(
			-Vector3(g.x, 0.25, g.z).normalized()),
			"a grazing sun projects at the clamped 0.25 vertical")


func test_high_sun_projection_uses_the_unclamped_tuple() -> void:
	var environment := _environment_at(1200, "NoonEnvironment")
	var light := SunShadow.new()
	light.set_environment_node(environment)
	add_child_autofree(light)

	var g := environment.get_light_direction()
	assert_gt(g.y, 0.25, "the noon fixture must sit above the clamp")
	var emission := -light.global_basis.z.normalized()
	assert_true(emission.is_equal_approx(-Vector3(g.x, g.y, g.z).normalized()),
			"above the clamp the presentation reduction (g2,g1,g0) passes through")
