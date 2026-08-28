extends GutTest


func _source(path: String) -> String:
	var file := FileAccess.open(path, FileAccess.READ)
	assert_not_null(file, "The production terrain shader source must be readable: %s" % path)
	return file.get_as_text() if file != null else ""


func _compact(source: String) -> String:
	return source.replace(" ", "").replace("\t", "").replace("\n", "").replace("\r", "")


func _light_alpha(normal_byte: Vector3, retail_getter_direction: Vector3) -> float:
	var light_byte := ShaderLightFixture.gpu_light_byte(retail_getter_direction)
	return clampf(4.0 * (normal_byte - Vector3(0.5, 0.5, 0.5)).dot(
		light_byte - Vector3(0.5, 0.5, 0.5)), 0.0, 1.0)


func _flat_ground_light_alpha(retail_getter_direction: Vector3) -> float:
	var normal_byte := ShaderLightFixture.quantize_retail_signed_vector(Vector3(0.0, 0.0, 1.0))
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
	assert_eq(ShaderLightFixture.gpu_light_byte(morning), Vector3(231, 83, 187) / 255.0,
		"08:00 D3DCOLOR diffuse RGB must be the witnessed getter permutation (z, x, y).")
	assert_almost_eq(_light_alpha(Vector3(217, 127, 217) / 255.0, morning),
		0.8987774, 0.000001, "08:00 X-ramp normal must receive the witnessed bright DOT3 response.")
	assert_almost_eq(_light_alpha(Vector3(127, 217, 217) / 255.0, morning),
		0.0794002, 0.000001, "08:00 Y-ramp normal must receive the witnessed dark DOT3 response.")


func test_detail_mips_sample_anisotropically_with_conservative_terminal_guard() -> void:
	# The witnessed device's anisotropic texfilter mode (MINFILTER=ANISOTROPIC
	# + MAXANISOTROPY) matches the retail reference captures. The shader's
	# longest-gradient scale is pinned only as a conservative guard that keeps
	# the synthetic 2x2/1x1 Godot tail unselectable; it is not an oracle for the
	# device's exact minor-axis/maximum-anisotropy LOD choice.
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
			"The conservative shader guard must keep requests out of the synthetic terminal tail.")
	assert_true(terrain.contains("textureGrad(source, uv, dx * gradient_scale, dy * gradient_scale)"),
		"Detail sampling must stay implicit/anisotropic within the retail chain.")


func test_detail_uv_uses_the_retail_512_unit_source_grid() -> void:
	# Retail writes UV1 from the parsed detail density divided by 512. Runtime
	# carries normalized coordinates for the full 1024 terrain atlas, so its
	# conversion must restore that factor of two. Stage 3 derives
	# its authored detail2/noise coordinates from the same retail UV1.
	# [orig: density parse @ 0x60f993..0x60f9b3; config field +0x1738 passed
	# @ 0x60e634; density/512 write @ 0x6029a0..0x6029aa; UV1 stores
	# @ 0x602db5..0x602dbe; stage-3 matrices @ 0x609786..0x609810]
	var shared := _compact(_source("res://shaders/terrain_lighting.gdshaderinc"))
	var runtime := _compact(_source("res://shaders/terrain.gdshader"))

	assert_true(
		shared.contains("vec2retail_detail_uv_from_atlas(vec2atlas_uv,floatdensity)") and
			shared.contains("returnatlas_uv*(density*2.0);"),
		"A 1024-atlas UV must convert to retail's density/512 detail coordinate.")
	assert_true(runtime.contains(
		"v_detail_uv=retail_detail_uv_from_atlas(UV,u_detail_density);"),
		"Runtime terrain must use the shared retail detail coordinate.")
	assert_true(shared.contains(
		"u_detail2,retail_detail_uv_from_atlas(colormap_uv,u_detail2_density)"),
		"The authored stage-3 detail must use density2/512 coordinates.")
	assert_true(shared.contains(
		"texture(u_water_noise,retail_detail_uv_from_atlas(colormap_uv,8.0))"),
		"The underwater stage-3 swap must use the retail 8/512 coordinate.")


func test_splat_modulation_is_the_second_detail_dp3() -> void:
	# The ps.1.4 splat's stage-3 dp3 input is the authored second detail pair
	# at its own density; maps without one run PS14Splat with no such stage.
	# The generated coefficient map belongs to the unported ps.1.1 tiers.
	# [orig: stage bind @ 0x6043ff; PS variant select @ 0x604544/0x6044e8;
	# texcoord transform density2/density @ 0x609810]
	var terrain := _source("res://shaders/terrain_lighting.gdshaderinc")
	assert_true(terrain.contains("u_detail2, retail_detail_uv_from_atlas("),
		"The second detail must sample at its own authored density.")
	assert_true(terrain.contains("normal_factor = dot(modulator, blend) * 2.0;"),
		"The stage-3 modulation is dot(stage-3 input, normalized blend) doubled.")
	assert_true(terrain.contains("float normal_factor = 1.0;"),
		"Maps without a second detail must run the PS14Splat variant (factor 1).")
	assert_false(terrain.contains("sample_detail_coefficient"),
		"The generated coefficient map must not modulate the ps.1.4 splat.")


func test_below_water_swaps_the_stage3_input_to_the_water_noise() -> void:
	# D-TERRAIN-8: underwater the LIVE stage-3 bind is the water noise at the
	# swapped 8/density texcoord (atlas colormap_uv * 16 after density cancels);
	# the dp3 modulation is then the caustic term. Detail2-less maps run
	# PS14Splat (no t3 consumer) and faithfully get NO underwater modulation.
	# [orig: live t3 slot swap @ 0x6043f2; selector @ 0x6044b1..0x604556;
	# 8/density texcoord @ 0x609786..0x6097D6]
	var terrain := _source("res://shaders/terrain_lighting.gdshaderinc")
	var compact := _compact(terrain)
	assert_true(compact.contains("uniformsampler2Du_water_noise"),
		"The live water noise texture must be a shared-include uniform.")
	assert_true(compact.contains("uniformboolu_below_water=false;"),
		"The below-water flag must default dry.")
	assert_true(compact.contains(
		"texture(u_water_noise,retail_detail_uv_from_atlas(colormap_uv,8.0))"),
		"The underwater stage-3 input samples at source*8/512 (atlas UV * 16).")
	assert_true(compact.contains("vec3modulator=u_below_water"),
		"The underwater swap must replace the dp3 INPUT, inside the detail2 gate.")
	# The swap lives inside the u_has_detail2 branch: no detail2, no modulation.
	var gate := terrain.find("if (u_has_detail2)")
	var swap := terrain.find("u_below_water\n\t\t\t? texture(u_water_noise")
	assert_gt(swap, gate, "The noise swap must sit inside the detail2 gate.")


func test_terrain_point_light_pool_is_the_two_stage_modulate2x_fold() -> void:
	# The pool's terrain leg: per patch the <= 16 rows ride one RGBAF texture
	# row per pool slot, each patch instance carries its slot, and the shader
	# sums stage 0 (the ground disc) x2 and stage 1 (the height strip) x2 over
	# the pixel constants that already carry the 0.5 — the explicit 2 * 2.
	# [orig: render_terrain_sector_batch @0x6092A0 per-light else-arm ->
	# Light_SetupTerrainProjectedPass @0x5AA830; textures Lighting_InitTextures
	# @0x5a94f0; the 0x600 shader's CLAMP address mode @0x5a98eb..0x5a98f4]
	var shared := _source("res://shaders/terrain_lighting.gdshaderinc")
	var compact := _compact(shared)
	assert_true(compact.contains(
		"uniformsampler2Du_terrain_light_rows:filter_nearest,repeat_disable,hint_default_black;"),
		"The rows texture must be fetched as texels (nearest, clamped, black when unbound).")
	assert_true(compact.contains(
		"uniformsampler2Du_terrain_light_disc:filter_linear,repeat_disable,hint_default_black;"),
		"The ground disc must sample bilinear + CLAMP, no mips (the 0x600 shader's address mode).")
	assert_true(compact.contains(
		"uniformsampler2Du_terrain_light_strip:filter_linear,repeat_disable,hint_default_black;"),
		"The height strip must sample bilinear + CLAMP, no mips.")
	assert_true(compact.contains("uniformboolu_terrain_light_enabled=false;"),
		"The pool leg must default off until the terrain binds its rows.")
	assert_true(compact.contains("constintTERRAIN_LIGHT_ROWS_PER_PATCH=16;"),
		"The per-patch cap is retail's 16-per-batch collect, not the object pass's 4.")
	assert_true(compact.contains("vec3terrain_point_light_pool(vec3world_pos,floatslot)"),
		"The pool sum must be the shared-include function both terrain shaders can call.")
	assert_true(compact.contains("vec2disc_uv=vec2(d.z,d.x)*posr.w+0.5;"),
		"The disc projection must be the mission (light.y - p.y, p.x - light.x) * inv + 0.5 contract in the Godot frame.")
	assert_true(compact.contains("vec2strip_uv=vec2(d.y*posr.w+0.5,0.5);"),
		"The strip projection must be the mission (p.z - light.z) * inv + 0.5 at v = 0.5.")
	assert_true(compact.contains("sum+=2.0*2.0*pixel.rgb*disc*strip;"),
		"Both stages are MODULATE2X: the explicit 2 * 2 over the 0.5-folded constants.")

	var runtime := _compact(_source("res://shaders/terrain.gdshader"))
	assert_true(runtime.contains("instanceuniformfloatu_instance_light_slot=0.0;"),
		"Each patch instance must carry the pool slot its rows live in.")
	assert_true(runtime.contains(
		"result+=terrain_point_light_pool(v_world_pos,u_instance_light_slot);"),
		"Runtime terrain must add the pool sum over the composed surface colour.")
	var pool := runtime.find("terrain_point_light_pool(v_world_pos")
	var fog := runtime.find("apply_terrain_fog(result")
	assert_gt(pool, 0, "The pool sum must be present in the runtime terrain shader.")
	assert_gt(fog, pool, "The pool sum is added before the fog mix so it fades with the batch.")


func test_runtime_uses_shared_tile_overlay_composition() -> void:
	var shared := _compact(_source("res://shaders/terrain_lighting.gdshaderinc"))
	var runtime := _compact(_source("res://shaders/terrain.gdshader"))

	assert_true(shared.contains("uniformsampler2Du_tile_overlay"),
		"The tile composite input must live in the shared surface include.")
	assert_true(shared.contains("vec4compose_retail_tile_overlay"),
		"Runtime must use the shared tile-composition implementation.")
	assert_true(runtime.contains("compose_retail_tile_overlay("),
		"Runtime terrain must use the shared tile-composition implementation.")
	# The environment binding must apply the retail tile tint through the
	# runtime terrain-uniform push.
	var environment := MissionEnvironment.new()
	add_child_autofree(environment)
	var material := ShaderMaterial.new()
	environment.apply_terrain_uniforms(material)
	assert_eq(material.get_shader_parameter("u_tile_overlay_tint"),
		environment.get_tile_overlay_tint(),
		"The environment binding must apply the retail tile tint in runtime.")
