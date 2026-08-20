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
	# The environment serves the direct retail getter tuple g; presentation
	# surface->light is the (g2, g1, g0) reduction, and the entity shadow
	# projection clamps the vertical component to 0.25 before negating
	# [orig: Environment_GetLightDirectionFloat @ 0x57d870;
	#  render_shadow_pass @ 0x5d7b70].
	var g := environment.get_light_direction()
	return -Vector3(g.z, maxf(g.y, 0.25), g.x).normalized()


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

	assert_true(light.visible, "a loaded environment enables the shadow light during ready")
	_assert_tracks_environment(light, environment,
			"ready publishes the first shadow direction before a process frame")


func test_environment_assignment_reorients_a_warm_shadow_synchronously() -> void:
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


func test_game_world_composes_only_the_live_shadow_map_during_ready() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	var data := EnvFile.new()
	data.reset_to_default()
	data.set_curtime(1500)
	var environment := world.get_node("MissionEnvironment") as MissionEnvironment
	environment.environment_data = data
	add_child_autofree(world)

	var live_shadow := world.get_node("SunShadow") as SunShadow
	_assert_tracks_environment(live_shadow, world.get_environment_node(),
			"GameWorld's live shadow is aligned when production composition returns")
	assert_false(world.has_node("NovaStaticSunShadow"),
		"Static terrain silhouettes are page alpha; no second shadow map remains live.")



func test_dynamic_projection_separates_live_casters_from_world_receivers() -> void:
	var light: SunShadow = SunShadow.new()
	light.projection_mode = SunShadow.PROJECTION_DYNAMIC
	add_child_autofree(light)

	assert_eq(light.light_cull_mask,
			Water.VISUAL_LAYER_WORLD
			| Water.VISUAL_LAYER_WORLD_NO_MIRROR
			| Water.VISUAL_LAYER_FP_BODY_SHADOW_ONLY,
			"live shadows reach both world-entity layers and the hidden FP body")
	assert_eq(light.shadow_caster_mask,
			Water.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER)
	assert_false(light.shadow_enabled,
			"the SlotShadow capture pipeline owns the entity ground shadows;"
			+ " the dynamic light keeps the direction law without a shadow map")


func test_low_sun_projection_clamps_the_vertical_component() -> void:
	# 06:30 sunrise: the getter tuple's vertical is ~0.1227, well under the
	# witnessed 0.25 slot-projection clamp, so retail projects entity shadows
	# as if the sun sat at ~14.5 deg — silhouettes never stretch past 4x
	# height [orig: render_shadow_pass @ 0x5d7b70 clamp; same constant as the
	# static collector @ 0x60d33f..0x60d341].
	var environment := _environment_at(630, "SunriseEnvironment")
	var light := SunShadow.new()
	light.set_environment_node(environment)
	add_child_autofree(light)

	var g := environment.get_light_direction()
	assert_lt(g.y, 0.25, "the sunrise fixture must sit under the clamp")
	var emission := -light.global_basis.z.normalized()
	assert_true(emission.is_equal_approx(
			-Vector3(g.z, 0.25, g.x).normalized()),
			"a grazing sun projects at the clamped 0.25 vertical")


func test_high_sun_projection_uses_the_unclamped_tuple() -> void:
	var environment := _environment_at(1200, "NoonEnvironment")
	var light := SunShadow.new()
	light.set_environment_node(environment)
	add_child_autofree(light)

	var g := environment.get_light_direction()
	assert_gt(g.y, 0.25, "the noon fixture must sit above the clamp")
	var emission := -light.global_basis.z.normalized()
	assert_true(emission.is_equal_approx(-Vector3(g.z, g.y, g.x).normalized()),
			"above the clamp the presentation reduction (g2,g1,g0) passes through")


func test_static_projection_only_reaches_the_reimpl_terrain_receiver() -> void:
	var light: SunShadow = SunShadow.new()
	light.projection_mode = SunShadow.PROJECTION_STATIC_TERRAIN
	add_child_autofree(light)

	assert_eq(light.light_cull_mask,
			Water.VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER)
	assert_eq(light.shadow_caster_mask,
			Water.VISUAL_LAYER_STATIC_SHADOW_CASTER)
	assert_true(light.shadow_enabled,
			"the static-terrain bake device still renders a shadow map")
