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


func test_fd_sampling_is_anisotropic_with_conservative_terminal_guard() -> void:
	# The device-global texfilter mode applies to every stage, and the retail
	# reference machine runs the anisotropic mode. The longest-gradient scale is
	# a conservative guard that keeps the synthetic Godot 2x2/1x1 tail past
	# retail's 4x4 terminal unselectable; it does not claim to reproduce the
	# device's exact anisotropic LOD footprint.
	# [orig: CGfxDevice_ApplyRenderStates per-stage loop @ 0x67e3b5..0x67e50e]
	var sampling := _source("res://shaders/foliage_fd_sampling.gdshaderinc")
	var detail := _source("res://shaders/foliage_detail.gdshaderinc")
	var silhouette := _source("res://shaders/foliage_silhouette.gdshader")

	assert_true(sampling.contains("floor(log2(max(min(dimensions.x, dimensions.y), 4.0))) - 2.0"),
		"The terminal LOD must match retail's final 4x4 level.")
	assert_true(sampling.contains("float gradient_scale = exp2(min(terminal_lod - requested_lod, 0.0));"),
			"The conservative shader guard must keep requests out of the synthetic terminal tail.")
	assert_true(sampling.contains("textureGrad(source, uv, dx * gradient_scale, dy * gradient_scale)"),
		":fd sampling must stay implicit/anisotropic within the retail chain.")
	assert_true(detail.contains("uniform sampler2D u_fd_texture : source_color, filter_linear_mipmap_anisotropic"),
		"Expanded detail must decode :fd color into linear HDR and sample anisotropically.")
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


func test_detail_foliage_uses_native_lighting_without_tile_alpha() -> void:
	var detail := _source("res://shaders/foliage_detail.gdshaderinc")
	var high := _source("res://shaders/foliage_detail_high.gdshader")
	var low := _source("res://shaders/foliage_detail_low.gdshader")
	assert_false(high.contains("unshaded"))
	assert_false(low.contains("unshaded"))
	assert_false(detail.contains("opennova_sun_direction"),
			"Native lights own the direct-light calculation.")
	assert_false(detail.contains("tile.a"),
			"The composed terrain page alpha must not act as baked foliage lighting.")
	assert_true(detail.contains("ALBEDO = color;"))
	assert_true(detail.contains("ROUGHNESS = 0.8;"))


func test_detail_foliage_uses_native_depth_fog() -> void:
	var detail := _source("res://shaders/foliage_detail.gdshaderinc")
	assert_false(detail.contains("opennova_fog_"))
	assert_false(detail.contains("apply_fog"))


func test_only_near_detail_foliage_submits_to_the_directional_caster_layer() -> void:
	var dispatcher := _source("res://src/terrain/nova_foliage_dispatcher.cpp")
	assert_true(dispatcher.contains(
			"detail ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON"))
	assert_true(dispatcher.contains(
			"detail ? Water::VISUAL_LAYER_DYNAMIC_SHADOW_CASTER : 0"),
			"Only the high-detail/near tier enters the sun's caster mask.")


func test_runtime_detail_foliage_consumes_terrains_resident_tile_page() -> void:
	var detail := _source("res://shaders/foliage_detail.gdshaderinc")
	var dispatcher := _source("res://src/terrain/nova_foliage_dispatcher.cpp")

	assert_true(
		detail.contains("uniform sampler2DArray u_tile_cache") and
			detail.contains("instance uniform bool u_instance_tile_cache_ready") and
			detail.contains("instance uniform float u_instance_tile_cache_layer") and
			detail.contains("instance uniform vec4 u_instance_tile_cache_origin_span"),
		"Detail foliage must expose the same composed-page array and per-draw page binding as terrain."
	)
	assert_true(
		detail.contains("texture(u_tile_cache, vec3(") and
			detail.contains("u_instance_tile_cache_layer"),
		"A ready foliage draw must sample its resident terrain-cache layer."
	)
	assert_true(
		detail.contains("vec2 half_texel = vec2(0.5 / 256.0);") and
			detail.contains("page_uv = clamp(page_uv, half_texel, vec2(1.0) - half_texel);"),
		"Foliage and terrain must share the page RT's half-texel edge clamp."
	)
	assert_true(
		dispatcher.contains("terrain_->get_tile_cache_texture()") and
			dispatcher.contains("terrain_->get_tile_cache_binding_for_world_point_native("),
		"Foliage must borrow both the Texture2DArray and the best-ready page binding from its owning Terrain."
	)
	assert_true(
		dispatcher.contains('"u_instance_tile_cache_ready"') and
			dispatcher.contains('"u_instance_tile_cache_layer"') and
			dispatcher.contains('"u_instance_tile_cache_origin_span"'),
		"The terrain-resident binding must reach every detail draw as instance state."
	)
	assert_true(
		detail.contains("if (u_has_tile_cache && u_instance_tile_cache_ready)") and
			detail.contains("if (u_has_colormap)"),
		"Missing runtime pages and ONED preview must retain the analytic terrain-surface fallback."
	)
