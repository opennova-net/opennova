extends GutTest



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


func test_static_projection_only_reaches_the_reimpl_terrain_receiver() -> void:
	var light: SunShadow = SunShadow.new()
	light.projection_mode = SunShadow.PROJECTION_STATIC_TERRAIN
	add_child_autofree(light)

	assert_eq(light.light_cull_mask,
			Water.VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER)
	assert_eq(light.shadow_caster_mask,
			Water.VISUAL_LAYER_STATIC_SHADOW_CASTER)
