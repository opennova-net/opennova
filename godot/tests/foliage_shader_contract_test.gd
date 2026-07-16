extends GutTest

# D3DCMP_GREATER accepts a texel only when source alpha is strictly greater
# than ALPHAREF. The equivalent shader discard boundary is therefore <=, not
# <; equality must fail in both fresh foliage tiers.


func _source(path: String) -> String:
	var file := FileAccess.open(path, FileAccess.READ)
	assert_not_null(file, "The production foliage shader source must be readable: %s" % path)
	return file.get_as_text() if file != null else ""


func test_alpha_test_rejects_samples_equal_to_reference() -> void:
	var detail := _source("res://shaders/foliage_detail.gdshaderinc")
	var silhouette := _source("res://shaders/foliage_silhouette.gdshader")

	assert_true(
		detail.contains("float retail_alpha = fd.a * u_fade;") and
			detail.contains("if (retail_alpha <= u_alpha_ref)"),
		"Detail must discard alpha == ALPHAREF (retail D3DCMP_GREATER)."
	)
	assert_true(
		silhouette.contains("fd.a <= u_alpha_ref"),
		"MODEL must discard alpha == ALPHAREF (retail D3DCMP_GREATER)."
	)


func test_detail_passes_blend_srcalpha_and_emulate_secondary_less() -> void:
	# Retail enables ALPHABLENDENABLE with SRCBLEND=SRCALPHA(5) and
	# DESTBLEND=INVSRCALPHA(6) in the detail technique block, so the blended
	# weight is exactly the tested alpha (t0.a * v0.a). The near secondary LOW
	# draw selects strict D3DCMP_LESS; the cutoff discard reproduces its texel
	# selection. [orig: Foliage_LoadDefAssets @ 0x60141f..0x601427;
	# Foliage_SetupFarSlotDraw @ 0x6008fc..0x600912]
	var detail := _source("res://shaders/foliage_detail.gdshaderinc")
	var high := _source("res://shaders/foliage_detail_high.gdshader")
	var low := _source("res://shaders/foliage_detail_low.gdshader")

	assert_true(detail.contains("ALPHA = clamp(retail_alpha, 0.0, 1.0);"),
		"Detail passes must blend at the retail SRCALPHA weight (fd.a * fade).")
	assert_true(
		detail.contains("u_high_pass_cutoff > 0.0 && retail_alpha > u_high_pass_cutoff"),
		"The near secondary LOW must skip texels the HIGH pass accepted (strict LESS).")
	assert_true(high.contains("blend_mix") and high.contains("depth_draw_always"),
		"HIGH blends SRCALPHA/INVSRCALPHA while alpha-test survivors write depth.")
	assert_true(low.contains("blend_mix") and low.contains("depth_draw_never"),
		"LOW blends SRCALPHA/INVSRCALPHA and never writes depth.")


func test_fd_sampling_is_anisotropic_with_retail_terminal_clamp() -> void:
	# The device-global texfilter mode applies to every stage, and the retail
	# reference machine runs the anisotropic mode; the synthetic Godot 2x2/1x1
	# tail past retail's 4x4 terminal stays unselectable via the gradient clamp.
	# [orig: CGfxDevice_ApplyRenderStates per-stage loop @ 0x67e3b5..0x67e50e]
	var sampling := _source("res://shaders/foliage_fd_sampling.gdshaderinc")
	var detail := _source("res://shaders/foliage_detail.gdshaderinc")
	var silhouette := _source("res://shaders/foliage_silhouette.gdshader")

	assert_true(sampling.contains("floor(log2(max(min(dimensions.x, dimensions.y), 4.0))) - 2.0"),
		"The terminal LOD must match retail's final 4x4 level.")
	assert_true(sampling.contains("float gradient_scale = exp2(min(terminal_lod - requested_lod, 0.0));"),
		"Requests past the retail terminal must scale gradients back onto it.")
	assert_true(sampling.contains("textureGrad(source, uv, dx * gradient_scale, dy * gradient_scale)"),
		":fd sampling must stay implicit/anisotropic within the retail chain.")
	assert_true(detail.contains("uniform sampler2D u_fd_texture : filter_linear_mipmap_anisotropic"),
		"Expanded detail must sample :fd anisotropically like the reference device.")
	assert_true(silhouette.contains("uniform sampler2D u_fd_texture : filter_linear_mipmap_anisotropic"),
		"MODEL foliage must sample :fd anisotropically like the reference device.")
	assert_true(detail.contains("sample_retail_foliage_fd(u_fd_texture, UV)"),
		"Expanded detail must use the retail-capped :fd sampler.")
	assert_true(silhouette.contains("sample_retail_foliage_fd(u_fd_texture, UV)"),
		"MODEL depth masks must use the same retail-capped :fd sampler.")


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


func test_detail_light_packs_world_sun_in_heightfield_texture_basis() -> void:
	var detail := _source("res://shaders/foliage_detail.gdshaderinc")
	assert_true(
		detail.contains("vec3 texture_basis_light = vec3("),
		"Detail must explicitly convert the retail getter tuple to GPU diffuse RGB."
	)
	assert_true(
		detail.contains("opennova_sun_direction.z,\n\t\t\topennova_sun_direction.x,\n\t\t\topennova_sun_direction.y"),
		"The GPU diffuse RGB order must be retail getter Z, X, Y."
	)
	assert_true(
		detail.contains("(texture_basis_light + 1.0) * 127.5"),
		"Retail byte packing must consume the converted GPU diffuse vector."
	)
	assert_false(
		detail.contains("(opennova_sun_direction + 1.0) * 127.5"),
		"The getter tuple must not be packed without the D3DCOLOR permutation."
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

func test_detail_fog_consumes_supplied_start_and_honors_disable() -> void:
	var detail := _source("res://shaders/foliage_detail.gdshaderinc")
	assert_true(
		detail.contains("if (opennova_fog_start == opennova_fog_end)"),
		"The device fog policy disables linear fog when start equals end."
	)
	assert_true(
		detail.contains("float start = opennova_fog_start;"),
		"The environment already supplies the authored per-type/per-overcast fog start."
	)
	assert_false(
		detail.contains("start = safe_end * 0.5") or
			detail.contains("start = safe_end * 0.25"),
		"The foliage shader must not overwrite the supplied type 2/3 fog start."
	)
