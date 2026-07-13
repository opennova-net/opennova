extends GutTest

# Source-level pins for the two foliage passes recovered from Jointops.exe.
# These deliberately name the shader inputs the dispatcher must bind: a
# runtime-less test can still catch the old shared-shader approximation.

const MODEL_SHADER_PATH := "res://shaders/foliage_model.gdshader"
const FAR_SHADER_PATH := "res://shaders/foliage_far.gdshader"
const WEATHER_PATH := "res://engine/environment/nova_weather.gd"


func _shader_source(path: String) -> String:
	assert_true(FileAccess.file_exists(path), "%s must exist" % path)
	return FileAccess.get_file_as_string(path)


func test_model_pass_uses_fd_alpha_and_black_diffuse() -> void:
	var source := _shader_source(MODEL_SHADER_PATH)
	assert_true(source.contains("uniform float u_model_alpha_ref"),
		"The host supplies the anchor-derived model alpha-test reference.")
	assert_true(source.contains("if (fd.a <= alpha_threshold)"),
		"Retail D3DCMP_GREATER rejects :fd alpha exactly equal to the MODEL reference.")
	assert_false(source.contains("ALPHA ="),
		"Retail MODEL foliage uses binary alpha test without enabling alpha blending.")
	assert_false(source.contains("ALPHA_SCISSOR_THRESHOLD"),
		"The explicit D3DCMP_GREATER discard is the complete opaque-cutout test.")
	assert_true(source.contains("vec3 result = vec3(0.0)"),
		"GridPlacementVS emits c6=(0,0,0,1), so the model pass starts black.")
	assert_false(source.contains("u_colormap"),
		"The model pass must not inherit the far-pass terrain texture combine.")
	assert_false(source.contains("u_terrain_tint"),
		"The disproven half-plus-bias tint approximation must stay removed.")
	assert_false(source.contains("u_weights_from_uv"),
		"Only model bound-square ground-fit weights belong in this shader.")
	assert_false(source.contains("opennova_fog"),
		"The model pass draws with D3DRS_FOGENABLE OFF - pass word 0x00440000 has no "
		+ "FOGENABLE bit and the apply latches fog unconditionally "
		+ "[orig: Foliage_DrawModelTileSlot @ 0x601e33; CGfxShader_ApplyPass @ 0x68324f].")


func test_far_pass_has_witnessed_wind_and_lighting_contract() -> void:
	var source := _shader_source(FAR_SHADER_PATH)
	assert_true(source.contains("uniform sampler2D u_terrain_light_texture"),
		"Far T1 is an explicit terrain light texture.")
	assert_true(source.contains("uniform vec4 u_far_pass_color"),
		"The host supplies retail c6 RGB and sector-fade alpha.")
	assert_true(source.contains("instance uniform float u_cell_fade"),
		"Each baked cell carries its own witnessed c6.a distance fade.")
	assert_true(source.contains("instance uniform float u_cell_alpha_ref"),
		"Each baked cell carries the witnessed high/low alpha-test ref (180 under 33, else 8).")
	assert_true(source.contains("global uniform float opennova_foliage_wave_addend"),
		"FAR foliage receives the scaled Env_WaveOscRing[0] weather global.")
	assert_true(source.contains(
		"float phase = world_pos.x + (u_far_wind_phase + opennova_foliage_wave_addend)"),
		"The ring-head addend joins GetTickCount*0.003 before the world-X phase term.")
	assert_true(source.contains("clamp(u_cell_alpha_ref, 8.0, 180.0)"),
		"The far alpha-test input is restricted to the witnessed low/high range.")
	assert_true(source.contains("float wind_weight = COLOR.r"),
		"Packed vertex red is wind weight, not terrain lighting.")
	assert_true(source.contains("global uniform vec3 opennova_sun_direction")
		and source.contains("COLOR.gba * 2.0 - 1.0")
		and source.contains("dot(ground_normal, opennova_sun_direction)"),
		"FAR foliage reconstructs retail patch-cache N dot L from the underlying terrain normal.")
	assert_true(source.contains("0.159155") and source.contains("6.2831898")
		and source.contains("-3.1415901") and source.contains("0.25"),
		"Far wind retains the retail phase-wrap constants.")
	assert_true(source.contains("-0.00000025239899")
		and source.contains("0.0000247609")
		and source.contains("-0.00138884")
		and source.contains("0.041666601")
		and source.contains("-0.5"),
		"Far wind retains the retail tenth-order cosine coefficients.")
	assert_true(source.contains("world_pos.z -= wind_weight * wave * 0.03"),
		"Retail render-Z wind maps to negative Godot world Z.")
	assert_true(source.contains("float far_alpha = fd.a * u_far_pass_color.a * u_cell_fade"),
		":fd alpha is modulated by the per-cell distance fade in c6.a.")
	assert_true(source.contains("if (far_alpha <= alpha_threshold)"),
		"Retail D3DCMP_GREATER rejects faded FAR alpha exactly equal to the pass reference.")
	assert_false(source.contains("ALPHA ="),
		"Retail FAR foliage uses binary alpha test without enabling alpha blending.")
	assert_false(source.contains("ALPHA_SCISSOR_THRESHOLD"),
		"The explicit D3DCMP_GREATER discard is the complete opaque-cutout test.")
	assert_true(source.contains("terrain_n_dot_l * opennova_sun_light + opennova_sky_ambient")
		and source.contains("fd.rgb * lit * u_far_pass_color.rgb * 8.0"),
		"The far fragment keeps the witnessed T0/T1/c0/c1/c6 multiply chain.")
	assert_false(source.contains("terrain_light.a * opennova_sun_light"),
		"A 24-bit colormap samples alpha=1; it cannot stand in for patch-cache N dot L.")
	assert_false(source.contains("u_terrain_tint"),
		"Far lighting never derives a half-plus-bias v0 from vertex color.")
	assert_false(source.contains("u_terrain_detail") or source.contains("u_terrain_blend"),
		"The host's terrain patch-cache RT stand-in stays colormap-only with no detail "
		+ "recompose; optional retail overlay/decal/scorch RGB remains D-FOLIAGE-7.")
	assert_true(source.contains("opennova_fog_color"),
		"FAR pass words 0x02460000/0x02560000 carry FOGENABLE - the far pass keeps its fog fold.")


func test_both_foliage_shaders_parse_as_shader_resources() -> void:
	assert_not_null(load(MODEL_SHADER_PATH) as Shader)
	assert_not_null(load(FAR_SHADER_PATH) as Shader)


func test_foliage_light_direction_tracks_the_active_sun_or_moon() -> void:
	var source := _shader_source(WEATHER_PATH)
	assert_true(source.contains(
		'global_shader_parameter_set(&"opennova_sun_direction", env.get_light_direction())'),
		"The foliage N dot L global must use the active sun/moon direction.")
	assert_false(source.contains(
		'global_shader_parameter_set(&"opennova_sun_direction", env.get_sun_direction())'),
		"Weather must not overwrite the active night light with the sun direction.")
