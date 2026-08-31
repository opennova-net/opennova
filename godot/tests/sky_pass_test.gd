extends GutTest

# C7 driver-contract pins for the recovered two-pass dome spec
# [orig: render_skybox @ 0x579080]. The per-fragment combine itself is shader
# code (verified by visual A/B); these pin what SkyPass pushes into it.

const FULL_00_ENV_FIXTURE := "res://../fixtures/env/synth_full.env"
const SKY_SHADER := "res://shaders/sky.gdshader"
const TICK := 1.0 / 62.0


func _make() -> Dictionary:
	var env_node: Node = MissionEnvironment.new()
	env_node.name = "SkyTestEnv"
	add_child_autofree(env_node)
	var env := EnvFile.new()
	env.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	env.load()
	env_node.environment_data = env
	var sky: Node = SkyPass.new()
	sky.environment_path = NodePath("../SkyTestEnv")
	add_child_autofree(sky)
	return {"sky": sky, "env_node": env_node, "env": env}


func test_keyframed_path_pushes_spec_uniforms() -> void:
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	var mat: ShaderMaterial = ctx.sky.get_sky_material()
	assert_eq(mat.get_shader_parameter("u_flat_pass"), false, "advanced clouds take the keyframed path")
	# The palette uniforms are source_color Colors now (the witnessed doubled
	# bytes ride Color components >1 unchanged; the hint decodes at bind).
	assert_eq(mat.get_shader_parameter("u_sky_base"), _as_color(ctx.env_node.get_sky_base() * 2.0), "c11 skybase")
	assert_eq(mat.get_shader_parameter("u_cloud_base"), _as_color(ctx.env_node.get_cloud_base() * 2.0), "c24 cloudbase")
	assert_eq(mat.get_shader_parameter("u_cloud_highlight"), _as_color(ctx.env_node.get_cloud_highlight() * 2.0), "c27 cloudhighlight")
	assert_eq(mat.get_shader_parameter("u_cloud_edge"), _as_color(ctx.env_node.get_cloud_edge() * 2.0), "c26 cloudedge")
	# The dome shader and the getters both serve GODOT-world vectors (the
	# env_axes.h swap applies once at each device seam — 2026-08-20
	# celestial-axis correction).
	assert_eq(mat.get_shader_parameter("u_sun_dir"),
		ctx.env_node.get_sun_direction(),
		"pass 1 is always sun-driven [orig: render_skybox @ 0x579287]")
	assert_eq(mat.get_shader_parameter("u_light_dir"),
		ctx.env_node.get_light_direction(),
		"pass 2 follows the active light [orig: render_skybox @ 0x579291]")
	assert_eq(mat.get_shader_parameter("u_fog_color"), _as_color(ctx.env_node.get_skyfog_color()),
		"the dome fogs with the dedicated skyfog block [orig: sky fog wrapper @ 0x579cb0]")
	assert_eq(float(mat.get_shader_parameter("u_fog_end")), ctx.env_node.get_fog_level(), "dome fog end distance")


func _as_color(v: Vector3) -> Color:
	return Color(v.x, v.y, v.z)


func test_keyframed_colors_use_retail_upload_scale_without_redoubling_fog() -> void:
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	var mat: ShaderMaterial = ctx.sky.get_sky_material()
	var actual := [
		_shader_color_units(mat, "u_sky_base"),
		_shader_color_units(mat, "u_sky_bright"),
		_shader_color_units(mat, "u_sky_highlight"),
		_shader_color_units(mat, "u_cloud_base"),
		_shader_color_units(mat, "u_cloud_highlight"),
		_shader_color_units(mat, "u_cloud_edge"),
		_shader_color_units(mat, "u_fog_color"),
	]
	# synth_full.env's 1200 keyframe (tests/fixtures/minimal_env_gen.cpp):
	# skybase 60,80,140; skybright 20,20,32; skyhighlight 150,160,150;
	# cloudbase 140,140,140; cloudhighlight 20,24,10; cloudedge 150,160,170;
	# skyfog 80,96,140 (its doubled blue saturates).
	assert_eq(actual, [
		[120, 160, 280],
		[40, 40, 64],
		[300, 320, 300],
		[280, 280, 280],
		[40, 48, 20],
		[300, 320, 340],
		[160, 192, 255],
	], "six sky/cloud constants use retail 2/255; active packed skyfog stays byte/255")


func test_weather_core_ticks_every_world_driven_sky_block_and_doubles_fog_afterward() -> void:
	var core := WeatherCore.new()
	var black := Color8(0, 0, 0)
	core.snap_colors(black, black, black, black)
	core.snap_sky_colors(black, black, black, black, black, black, black, black, black, black)
	core.set_sky_color_targets(
		Color8(8, 16, 24),
		Color8(16, 32, 48),
		Color8(24, 40, 56),
		Color8(32, 64, 96),
		Color8(40, 72, 104),
		Color8(48, 80, 112),
		Color8(56, 88, 120),
		Color8(64, 96, 128),
		Color8(72, 104, 136),
		Color8(80, 112, 144))
	core.tick(black, black, Color8(200, 104, 48), black, Color.WHITE, 0.0)

	assert_eq(_color_units(core.get_fog()), [50, 26, 12],
		"fog smooths in authored bytes before the saturating x2 render tail")
	assert_eq(_color_units(core.get_skyfog()), [2, 4, 6],
		"skyfog smooths, horizon-blends, then doubles (no blend at 1024)")
	assert_eq(_color_units(core.get_ceiling()), [2, 4, 6])
	assert_eq(_color_units(core.get_cloud()), [3, 5, 7])
	assert_eq(_color_units(core.get_floor()), [4, 8, 12])
	assert_eq(_color_units(core.get_skybase()), [5, 9, 13])
	assert_eq(_color_units(core.get_skybright()), [6, 10, 14])
	assert_eq(_color_units(core.get_skyhighlight()), [7, 11, 15])
	assert_eq(_color_units(core.get_cloudbase()), [8, 12, 16])
	assert_eq(_color_units(core.get_cloudhighlight()), [9, 13, 17])
	assert_eq(_color_units(core.get_cloudedge()), [10, 14, 18])


func test_weather_writes_all_dome_colors_back_to_environment() -> void:
	var ctx := _make()
	var weather := Weather.new()
	weather.environment_path = ctx.env_node.get_path()
	add_child_autofree(weather)
	weather.advance_frame(TICK)

	assert_eq(ctx.env_node.get_skyfog_color(), weather.get_smooth_skyfog())
	assert_eq(ctx.env_node.get_ceiling_color(), weather.get_smooth_ceiling())
	assert_eq(ctx.env_node.get_cloud_tint(), weather.get_smooth_cloud())
	assert_eq(ctx.env_node.get_floor_color(), weather.get_smooth_floor())
	assert_eq(ctx.env_node.get_sky_base(), weather.get_smooth_sky_base())
	assert_eq(ctx.env_node.get_sky_bright(), weather.get_smooth_sky_bright())
	assert_eq(ctx.env_node.get_sky_highlight(), weather.get_smooth_sky_highlight())
	assert_eq(ctx.env_node.get_cloud_base(), weather.get_smooth_cloud_base())
	assert_eq(ctx.env_node.get_cloud_highlight(), weather.get_smooth_cloud_highlight())
	assert_eq(ctx.env_node.get_cloud_edge(), weather.get_smooth_cloud_edge())


func test_dome_fog_uses_skyfog_instead_of_world_fog() -> void:
	var ctx := _make()
	for keyframe in ctx.env.get_tod_keyframes():
		keyframe.set_fog_color(Color8(10, 20, 30))
		keyframe.set_skyfog_color(Color8(40, 50, 60))
	ctx.env.set_fog_level(1024.0)
	ctx.sky.advance_frame(TICK)

	var dome_fog: Color = ctx.sky.get_sky_material().get_shader_parameter("u_fog_color")
	var skyfog: Vector3 = ctx.env_node.get_skyfog_color()
	var world_fog: Vector3 = ctx.env_node.get_fog_color()
	assert_eq(dome_fog, Color(skyfog.x, skyfog.y, skyfog.z))
	assert_ne(dome_fog, Color(world_fog.x, world_fog.y, world_fog.z),
		"the sky wrapper swaps to skyfog while the world keeps ordinary fog")


func test_underwater_dome_keeps_the_smoothed_fog_end_without_rewriting_sky_wrapper() -> void:
	# render_skybox loads c9.x from the raw smoothed Env_FogDistCurrent with no
	# underwater leg (env-tod-re.md, the VS-constant table), so the dome's fog
	# end is the same unscaled distance on both sides of the water plane while
	# the world passes fog to the murk-derived end.
	var ctx := _make()
	ctx.sky.advance_frame(TICK)
	var mat: ShaderMaterial = ctx.sky.get_sky_material()
	var dry_sky_base: Color = mat.get_shader_parameter("u_sky_base")
	var dry_fog_end := float(mat.get_shader_parameter("u_fog_end"))
	assert_almost_eq(dry_fog_end, ctx.env_node.get_fog_level(), 0.001,
			"the dome fog end is the raw smoothed distance above water")
	ctx.env_node.set_underwater_view(true)
	ctx.sky.advance_frame(TICK)

	var underwater_skyfog: Vector3 = ctx.env_node.get_skyfog_color()
	assert_eq(mat.get_shader_parameter("u_fog_color"),
			Color(underwater_skyfog.x, underwater_skyfog.y, underwater_skyfog.z),
			"the dome keeps the witnessed skyfog wrapper underwater")
	assert_almost_eq(float(mat.get_shader_parameter("u_fog_end")), dry_fog_end, 0.001,
			"the dome keeps the unscaled smoothed fog end underwater")
	assert_true(absf(ctx.env_node.get_scene_fog_end() - dry_fog_end) > 1.0,
			"the world's murk end differs from the dome's, so the pin is live")
	assert_eq(mat.get_shader_parameter("u_sky_base"), dry_sky_base,
			"pass fog selection does not rewrite authored sky/TOD colors")


func _shader_color_units(material: ShaderMaterial, parameter: StringName) -> Array[int]:
	var value: Color = material.get_shader_parameter(parameter)
	return [roundi(value.r * 255.0), roundi(value.g * 255.0), roundi(value.b * 255.0)]


func _color_units(value: Color) -> Array[int]:
	return [roundi(value.r * 255.0), roundi(value.g * 255.0), roundi(value.b * 255.0)]


func test_flat_pass_does_not_stuff_keyframed_uniforms() -> void:
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	var keyframed_base: Color = ctx.sky.get_sky_material().get_shader_parameter("u_sky_base")
	ctx.env.set_advanced_clouds(0)
	ctx.sky.advance_frame(0.016)
	var mat: ShaderMaterial = ctx.sky.get_sky_material()
	assert_eq(mat.get_shader_parameter("u_flat_pass"), true, "advanced_clouds 0 takes the flat pass")
	var cloud_tint: Vector3 = ctx.env_node.get_cloud_tint()
	assert_eq(mat.get_shader_parameter("u_flat_color"),
		Color(cloud_tint.x, cloud_tint.y, cloud_tint.z),
		"the flat dome color is cloud_rgb [orig: render_skybox @ 0x579b42]")
	assert_eq(mat.get_shader_parameter("u_sky_base"), keyframed_base,
		"the flat pass no longer overwrites the keyframed uniforms")

func test_cloud_textures_rebind_and_clear_after_environment_edits() -> void:
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	var mat: ShaderMaterial = ctx.sky.get_sky_material()
	var before: Texture2D = mat.get_shader_parameter("u_cloud_tex1")
	assert_not_null(before, "fixture starts with a bound cloud layer")

	ctx.env.set_sky_map1(ctx.env.get_sky_map2())
	ctx.sky.advance_frame(0.016)
	var after: Texture2D = mat.get_shader_parameter("u_cloud_tex1")
	assert_ne(after, before,
			"editing a cloud map refreshes the live sky binding")

	ctx.env.set_sky_map1("no_such_cloud_a.pcx")
	ctx.env.set_sky_map2("no_such_cloud_b.pcx")
	ctx.sky.advance_frame(0.016)
	assert_eq(mat.get_shader_parameter("u_has_clouds"), false,
			"removing both maps disables stale cloud sampling")


func test_sky_background_evaluates_the_witnessed_dome_analytically() -> void:
	# ADR 0043 d3 amendment: the dome mesh retired into a real Sky background.
	# The shader must carry the builder's exact surface constants (the engine
	# oracle env::sky_dome_intersect + its ctest sweep prove the equivalence)
	# and the witnessed anchor law as the dome-space eye height.
	var shader := load(SKY_SHADER) as Shader
	var code := shader.code
	assert_true(code.contains("shader_type sky;"),
			"the sky is the scene's Sky background, not a mesh")
	assert_true(code.contains("float o_y = eye_world_y * 0.5;"),
			"the anchor law survives as the dome-space eye height "
			+ "[orig: render_skybox @ 0x5790d0]")
	assert_true(code.contains("sqrt(8388608.0)"),
			"the cap-sphere C = sqrt(2^23) [orig: @ 0x7d75e0]")
	assert_true(code.contains("9437184.0"),
			"R^2 = 3072^2 [orig: @ 0x7d75c8]")
	assert_true(code.contains("h.xz * 0.003125"),
			"layer-1 planar cloud UVs at 1/320 [orig: kUv1Scale @ 0x7d75d0]")
	assert_true(code.contains("h.xz * 0.00146484375"),
			"layer-2 planar cloud UVs at 3/2048 [orig: kUv2Scale @ 0x7d75cc]")
	assert_true(code.contains("normalize(vec3(h.x, h.y / (s * s), h.z))"),
			"the builder's anisotropic dome normal [orig: @ 0x578fbb]")


func test_proximity_is_the_tracked_world_space_approximation() -> void:
	# The original dp3'd normalized CLIP-space vectors [orig: c14 upload
	# @ 0x57960e..0x579641]; sky shaders expose no projection, so the
	# world-space dot stands in — a tracked divergence (register MP-8).
	var shader := load(SKY_SHADER) as Shader
	var code := shader.code
	assert_true(code.contains("max(dot(eyedir, normalize(dir)), 0.0)"),
			"proximity is the world-space dot approximation")
	assert_false(code.contains("clip.w - clip.z"),
			"the clip-space depth conversion died with the mesh")


func test_frame_clear_and_suppression_states_live_in_the_sky() -> void:
	# The witnessed 3-state clear fills the open below-rim region; the blink
	# 0x2 indoors gate suppresses the whole visible pass; the radiance keeps
	# the sky and serves the hemi ground below the horizon.
	var shader := load(SKY_SHADER) as Shader
	var code := shader.code
	assert_true(code.contains("u_frame_clear"),
			"the 3-state clear paints misses and the suppressed sky")
	assert_true(code.contains("u_sky_suppressed"), "the blink 0x2 gate")
	assert_true(code.contains("AT_CUBEMAP_PASS"),
			"the radiance branch ignores suppression")
	assert_true(code.contains("COLOR = u_hemi_ground;"),
			"the radiance lower hemisphere is the .env ground color")
	assert_true(code.contains("uniform bool u_sky_suppressed = true;"),
			"boot default: suppressed (idle black) until the world writes")


func test_cloud_tint_uniform_is_gone_from_the_shader() -> void:
	var shader := load(SKY_SHADER) as Shader
	assert_false(shader.code.contains("u_cloud_tint"),
		"the fabricated keyframed-path cloud tint is deleted (divergence #20 fix)")
	assert_false(shader.code.contains("u_moon_dir"),
		"sun/moon glow terms are deleted - celestial bodies are Celestial's job")


func test_cloud_layers_keep_the_recovered_anisotropic_stage_filter() -> void:
	var shader := load(SKY_SHADER) as Shader
	assert_eq(shader.code.count("filter_linear_mipmap_anisotropic"), 2,
		"both active cloud stages use the reference device's anisotropic minification")


func test_scroll_offsets_come_from_the_weather_core() -> void:
	var ctx := _make()
	var weather: Node3D = Weather.new()
	weather.environment_path = ctx.env_node.get_path()
	add_child_autofree(weather)
	ctx.sky.weather_path = weather.get_path()
	for _i in 8:
		weather.advance_frame(0.016)
	ctx.sky.advance_frame(0.016)
	var off1: Vector2 = ctx.sky.get_sky_material().get_shader_parameter("u_scroll_offset1")
	var off2: Vector2 = ctx.sky.get_sky_material().get_shader_parameter("u_scroll_offset2")
	assert_eq(off1, weather.get_cloud_uv_offset1(0.0, 0.0),
		"sky reads layer 1 from the weather core [orig: @ 0x57f1a5]")
	assert_eq(off2, weather.get_cloud_uv_offset2(0.0, 0.0),
		"sky reads layer 2 from the weather core")
	# The accumulator rides U NEGATIVELY, V positively (no camera here, so the
	# offsets are the pure accumulator terms) [orig: render_skybox @ 0x5791de].
	assert_lt(off1.x, 0.0, "layer-1 U accumulator term is negative (env #26)")
	assert_gt(off1.y, 0.0, "layer-1 V accumulator term is positive")


func test_scroll_falls_back_to_a_private_core_without_weather() -> void:
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	var first: Vector2 = ctx.sky.get_sky_material().get_shader_parameter("u_scroll_offset1")
	ctx.sky.advance_frame(0.016)
	var second: Vector2 = ctx.sky.get_sky_material().get_shader_parameter("u_scroll_offset2")
	var off1: Vector2 = ctx.sky.get_sky_material().get_shader_parameter("u_scroll_offset1")
	assert_ne(off1, first, "the fallback core keeps ticking the accumulators")
	assert_lt(off1.x, 0.0, "fallback layer-1 U is negative too")
	assert_ne(second, Vector2.ZERO, "layer 2 advances as well")
