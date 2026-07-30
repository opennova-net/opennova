extends Node3D

const OUT_DIR_ENV := "NOVA_SHADOW_PROBE_DIR"

var _light: DirectionalLight3D


func _ready() -> void:
	DisplayServer.window_set_size(Vector2i(640, 480))
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 5.5, 8.0)
	camera.current = true
	add_child(camera)
	camera.look_at(Vector3.ZERO, Vector3.UP)

	var world_env := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.75, 0.75, 0.75)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color.WHITE
	environment.ambient_light_energy = 1.0
	world_env.environment = environment
	add_child(world_env)

	var receiver := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(10.0, 10.0)
	receiver.mesh = plane
	receiver.layers = NovaWater.VISUAL_LAYER_WORLD \
			| NovaWater.VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER
	var base := StandardMaterial3D.new()
	base.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	base.albedo_color = Color(0.65, 0.65, 0.65)
	var catcher := ShaderMaterial.new()
	catcher.shader = load("res://shaders/sun_shadow_catcher.gdshader")
	base.next_pass = catcher
	receiver.material_override = base
	add_child(receiver)

	var caster := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(2.0, 2.0, 2.0)
	caster.mesh = box
	caster.position = Vector3(0.0, 1.0, 0.0)
	var caster_material := StandardMaterial3D.new()
	caster_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	caster_material.albedo_color = Color(0.35, 0.2, 0.15)
	caster.material_override = caster_material
	caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY
	caster.layers = NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER
	add_child(caster)

	_light = DirectionalLight3D.new()
	_light.light_energy = 1.0
	_light.shadow_enabled = false
	_light.light_cull_mask = NovaWater.VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER
	_light.shadow_caster_mask = NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER
	_light.directional_shadow_max_distance = 32.0
	_light.shadow_bias = 0.02
	_light.shadow_normal_bias = 0.2
	add_child(_light)
	# DirectionalLight3D emits along local -Z.
	_light.look_at(_light.global_position + Vector3(-0.6, -1.0, -0.4).normalized(),
			Vector3.UP)
	var dynamic_light := DirectionalLight3D.new()
	dynamic_light.light_energy = 1.0
	dynamic_light.shadow_enabled = true
	dynamic_light.light_cull_mask = NovaWater.VISUAL_LAYER_WORLD
	dynamic_light.shadow_caster_mask = \
			NovaWater.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER
	add_child(dynamic_light)
	dynamic_light.look_at(
			dynamic_light.global_position + Vector3(-0.6, -1.0, -0.4).normalized(),
			Vector3.UP)

	await get_tree().process_frame
	await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var unshadowed := get_viewport().get_texture().get_image()
	_light.shadow_enabled = true
	for _i in range(8):
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var shadowed := get_viewport().get_texture().get_image()

	var changed := 0
	var sum_darkening := 0.0
	var max_darkening := 0.0
	for y in range(shadowed.get_height()):
		for x in range(shadowed.get_width()):
			var before := unshadowed.get_pixel(x, y).get_luminance()
			var after := shadowed.get_pixel(x, y).get_luminance()
			var darkening := before - after
			if darkening > 0.02:
				changed += 1
				sum_darkening += darkening
				max_darkening = maxf(max_darkening, darkening)
	print("[shadow-catcher] changed=%d mean=%.4f max=%.4f" % [
			changed,
			sum_darkening / float(maxi(changed, 1)),
			max_darkening])
	var output_dir := OS.get_environment(OUT_DIR_ENV)
	if not output_dir.is_empty():
		DirAccess.make_dir_recursive_absolute(output_dir)
		unshadowed.save_png(output_dir.path_join("unshadowed.png"))
		shadowed.save_png(output_dir.path_join("shadowed.png"))
	if changed < 100 or max_darkening < 0.1:
		push_error("[shadow-catcher] FAIL: no useful cast-shadow response")
		get_tree().quit(1)
		return
	print("[shadow-catcher] PASS")
	get_tree().quit(0)
