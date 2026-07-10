extends GutTest

# Source-level pins for the two foliage passes recovered from Jointops.exe.
# These deliberately name the shader inputs the dispatcher must bind: a
# runtime-less test can still catch the old shared-shader approximation.

const MODEL_SHADER_PATH := "res://shaders/foliage_model.gdshader"
const FAR_SHADER_PATH := "res://shaders/foliage_far.gdshader"


func _shader_source(path: String) -> String:
	assert_true(FileAccess.file_exists(path), "%s must exist" % path)
	return FileAccess.get_file_as_string(path)


func test_model_pass_uses_fd_alpha_and_black_diffuse() -> void:
	var source := _shader_source(MODEL_SHADER_PATH)
	assert_true(source.contains("uniform float u_model_alpha_ref"),
		"The host supplies the anchor-derived model alpha-test reference.")
	assert_true(source.contains("ALPHA = fd.a"), ":fd supplies model alpha.")
	assert_true(source.contains("vec3 result = vec3(0.0)"),
		"GridPlacementVS emits c6=(0,0,0,1), so the model pass starts black.")
	assert_false(source.contains("u_colormap"),
		"The model pass must not inherit the far-pass terrain texture combine.")
	assert_false(source.contains("u_terrain_tint"),
		"The disproven half-plus-bias tint approximation must stay removed.")
	assert_false(source.contains("u_weights_from_uv"),
		"Only model bound-square ground-fit weights belong in this shader.")


func test_far_pass_has_witnessed_wind_and_lighting_contract() -> void:
	var source := _shader_source(FAR_SHADER_PATH)
	assert_true(source.contains("uniform sampler2D u_terrain_light_texture"),
		"Far T1 is an explicit terrain light texture.")
	assert_true(source.contains("uniform vec4 u_far_pass_color"),
		"The host supplies retail c6 RGB and sector-fade alpha.")
	assert_true(source.contains("uniform float u_far_alpha_ref"),
		"The input can represent the witnessed low/high alpha refs (8/180); selector parity remains D-FOLIAGE-7.")
	assert_true(source.contains("global uniform float opennova_foliage_wave_addend"),
		"FAR foliage receives the scaled Env_WaveOscRing[0] weather global.")
	assert_true(source.contains(
		"float phase = world_pos.x + (u_far_wind_phase + opennova_foliage_wave_addend)"),
		"The ring-head addend joins GetTickCount*0.003 before the world-X phase term.")
	assert_true(source.contains("hint_range(8.0, 180.0")
		and source.contains("clamp(u_far_alpha_ref, 8.0, 180.0)"),
		"The far alpha-test input is restricted to the witnessed low/high range.")
	assert_true(source.contains("float wind_weight = COLOR.r"),
		"Packed vertex red is wind weight, not terrain lighting.")
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
	assert_true(source.contains("ALPHA = fd.a * u_far_pass_color.a"),
		":fd alpha is modulated by the sector fade in c6.a.")
	assert_true(source.contains("terrain_light.a * opennova_sun_light + opennova_sky_ambient")
		and source.contains("fd.rgb * lit * u_far_pass_color.rgb * 8.0"),
		"The far fragment keeps the witnessed T0/T1/c0/c1/c6 multiply chain.")
	assert_false(source.contains("u_terrain_tint"),
		"Far lighting never derives a half-plus-bias v0 from vertex color.")


func test_both_foliage_shaders_parse_as_shader_resources() -> void:
	assert_not_null(load(MODEL_SHADER_PATH) as Shader)
	assert_not_null(load(FAR_SHADER_PATH) as Shader)
