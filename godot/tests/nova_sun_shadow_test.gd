extends GutTest

const NovaSunShadowScript := preload(
		"res://engine/environment/nova_sun_shadow.gd")


func test_dynamic_projection_separates_live_casters_from_world_receivers() -> void:
	var light: NovaSunShadow = NovaSunShadowScript.new()
	light.projection_mode = NovaSunShadow.PROJECTION_DYNAMIC
	add_child_autofree(light)

	assert_eq(light.light_cull_mask,
			NovaWater.VISUAL_LAYER_WORLD
			| NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY)
	assert_eq(light.shadow_caster_mask,
			NovaWater.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER)


func test_static_projection_only_reaches_the_reimpl_terrain_receiver() -> void:
	var light: NovaSunShadow = NovaSunShadowScript.new()
	light.projection_mode = NovaSunShadow.PROJECTION_STATIC_TERRAIN
	add_child_autofree(light)

	assert_eq(light.light_cull_mask,
			NovaWater.VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER)
	assert_eq(light.shadow_caster_mask,
			NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER)
