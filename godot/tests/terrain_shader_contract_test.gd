extends GutTest


func _source(path: String) -> String:
	var file := FileAccess.open(path, FileAccess.READ)
	assert_not_null(file, "The production terrain shader source must be readable: %s" % path)
	return file.get_as_text() if file != null else ""


func _compact(source: String) -> String:
	return source.replace(" ", "").replace("\t", "").replace("\n", "").replace("\r", "")


func _quantize_retail_signed_vector(value: Vector3) -> Vector3:
	return Vector3(
		floorf(clampf((value.x + 1.0) * 127.5, 0.0, 255.0)),
		floorf(clampf((value.y + 1.0) * 127.5, 0.0, 255.0)),
		floorf(clampf((value.z + 1.0) * 127.5, 0.0, 255.0))
	) / 255.0


func _gpu_light_byte(retail_getter_direction: Vector3) -> Vector3:
	# PolyTrn packs the getter tuple into D3DCOLOR diffuse RGB as (z, x, y).
	var gpu_diffuse_rgb := Vector3(
		retail_getter_direction.z,
		retail_getter_direction.x,
		retail_getter_direction.y
	)
	return _quantize_retail_signed_vector(gpu_diffuse_rgb)


func _light_alpha(normal_byte: Vector3, retail_getter_direction: Vector3) -> float:
	var light_byte := _gpu_light_byte(retail_getter_direction)
	return clampf(4.0 * (normal_byte - Vector3(0.5, 0.5, 0.5)).dot(
		light_byte - Vector3(0.5, 0.5, 0.5)), 0.0, 1.0)


func _flat_ground_light_alpha(retail_getter_direction: Vector3) -> float:
	var normal_byte := _quantize_retail_signed_vector(Vector3(0.0, 0.0, 1.0))
	return _light_alpha(normal_byte, retail_getter_direction)


func test_terrain_tile_light_uses_heightfield_texture_basis() -> void:
	var terrain := _source("res://shaders/terrain_lighting.gdshaderinc")
	var compact := _compact(terrain)
	assert_true(
		compact.contains(
			"vec3texture_basis_light=vec3(u_sun_direction.z,u_sun_direction.x,u_sun_direction.y);"
		),
		"Terrain must pack the retail getter tuple into GPU diffuse RGB order."
	)
	assert_true(
		compact.contains("quantize_retail_signed_vector(texture_basis_light)"),
		"Terrain DOT3 must byte-pack the converted GPU diffuse light vector."
	)
	assert_false(
		compact.contains("quantize_retail_signed_vector(u_sun_direction)"),
		"Terrain must not byte-pack the getter tuple without the D3DCOLOR permutation."
	)

	var env := EnvFile.new()
	var dawn := _flat_ground_light_alpha(env.compute_sun_direction(600.0))
	var noon := _flat_ground_light_alpha(env.compute_sun_direction(1200.0))
	var dusk := _flat_ground_light_alpha(env.compute_sun_direction(1800.0))
	assert_lt(dawn, 0.01, "Flat ground must not receive overhead DOT3 light at dawn.")
	assert_gt(noon, 0.9, "Flat ground must receive overhead DOT3 light at noon.")
	assert_lt(dusk, 0.01, "Flat ground must not receive overhead DOT3 light at dusk.")

	var morning := env.compute_sun_direction(800.0)
	assert_eq(_gpu_light_byte(morning), Vector3(231, 83, 187) / 255.0,
		"08:00 D3DCOLOR diffuse RGB must be the witnessed getter permutation (z, x, y).")
	assert_almost_eq(_light_alpha(Vector3(217, 127, 217) / 255.0, morning),
		0.8987774, 0.000001, "08:00 X-ramp normal must receive the witnessed bright DOT3 response.")
	assert_almost_eq(_light_alpha(Vector3(127, 217, 217) / 255.0, morning),
		0.0794002, 0.000001, "08:00 Y-ramp normal must receive the witnessed dark DOT3 response.")


func test_detail_mips_sample_anisotropically_with_terminal_clamp() -> void:
	# The witnessed device's anisotropic texfilter mode (MINFILTER=ANISOTROPIC
	# + MAXANISOTROPY) matches the retail reference captures: minor-axis LOD
	# keeps ground detail near mip 0 at grazing angles instead of washing to
	# the paired far texture. The synthetic 2x2/1x1 Godot tail must stay
	# unselectable via the terminal gradient clamp.
	# [orig: per-stage filter select @ 0x67e38a..0x67e45b]
	var terrain := _source("res://shaders/terrain_lighting.gdshaderinc")
	assert_true(
		terrain.contains("u_detail_c1 : filter_linear_mipmap_anisotropic") and
			terrain.contains("u_detail_c2 : filter_linear_mipmap_anisotropic") and
			terrain.contains("u_detail_c3 : filter_linear_mipmap_anisotropic") and
			terrain.contains("u_detail2 : filter_linear_mipmap_anisotropic") and
			terrain.contains("u_colormap : filter_linear_mipmap_anisotropic") and
			terrain.contains("u_blendmap : filter_linear_mipmap_anisotropic"),
		"Every mipped terrain input must sample anisotropically like the retail reference.")
	assert_true(terrain.contains("float gradient_scale = exp2(min(terminal_lod - requested_lod, 0.0));"),
		"Requests past the 4x4 retail terminal must scale gradients back onto it.")
	assert_true(terrain.contains("textureGrad(source, uv, dx * gradient_scale, dy * gradient_scale)"),
		"Detail sampling must stay implicit/anisotropic within the retail chain.")


func test_splat_modulation_is_the_second_detail_dp3() -> void:
	# The ps.1.4 splat's stage-3 dp3 input is the authored second detail pair
	# at its own density; maps without one run PS14Splat with no such stage.
	# The generated coefficient map belongs to the unported ps.1.1 tiers.
	# [orig: stage bind @ 0x6043ff; PS variant select @ 0x604544/0x6044e8;
	# texcoord transform density2/density @ 0x609810]
	var terrain := _source("res://shaders/terrain_lighting.gdshaderinc")
	assert_true(terrain.contains("u_detail2, colormap_uv * u_detail2_density"),
		"The second detail must sample at its own authored density.")
	assert_true(terrain.contains("normal_factor = dot(detail2, blend) * 2.0;"),
		"The stage-3 modulation is dot(second detail, normalized blend) doubled.")
	assert_true(terrain.contains("float normal_factor = 1.0;"),
		"Maps without a second detail must run the PS14Splat variant (factor 1).")
	assert_false(terrain.contains("sample_detail_coefficient"),
		"The generated coefficient map must not modulate the ps.1.4 splat.")


func test_runtime_and_oned_share_tile_overlay_composition() -> void:
	var shared := _compact(_source("res://shaders/terrain_lighting.gdshaderinc"))
	var runtime := _compact(_source("res://shaders/terrain.gdshader"))
	var editor := _compact(_source("res://shaders/terrain_editor.gdshader"))
	var environment := _compact(_source("res://engine/environment/nova_environment.gd"))

	assert_true(shared.contains("uniformsampler2Du_tile_overlay"),
		"The tile composite input must live in the surface shader shared by runtime and ONED.")
	assert_true(shared.contains("vec4compose_retail_tile_overlay"),
		"Runtime and ONED must use one tile-composition implementation.")
	assert_true(runtime.contains("compose_retail_tile_overlay("),
		"Runtime terrain must use the shared tile-composition implementation.")
	assert_true(editor.contains("compose_retail_tile_overlay("),
		"ONED terrain must composite mission tiles in its terrain material.")
	assert_true(environment.contains("set_shader_parameter(\"u_tile_overlay_tint\",get_tile_overlay_tint())"),
		"The shared environment binding must apply the retail tile tint in runtime and ONED.")
