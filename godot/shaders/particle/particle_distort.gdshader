shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Distort (7).
// Retail decodes the type-7 atlas texel as a normal (blue height -> wrapped
// gradient at 1/32 scale with blue forced to 255 — applied when the flipbook
// strip is composed, engine/runtime/particle/height_to_normal), combines it
// with a rotating 1/51.2 perturbation scaled by the particle alpha, and
// samples its captured scene copy at that offset; the sample replaces the
// destination weighted by particle x texture alpha. Retail captured AFTER the
// world render, but Godot's screen texture is the OPAQUE-pass copy — a
// replacement here erases the transparent geometry (water, foliage cards)
// already blended under the quad. The witnessed UV equation stays; the
// composition becomes a screen-space DELTA over the live destination:
//   out = dst + w * (screen(scene_uv) - screen(SCREEN_UV))
// via blend_premul_alpha (out = ALBEDO + dst * (1 - ALPHA); ALPHA = 0 — the
// water.gdshader premul identity), with the witnessed weight
// w = particle alpha * texel alpha folded into ALBEDO because the premul
// blender applies no SRC_ALPHA factor. Where dst is the opaque backdrop this
// reproduces the replacement (dst ~= screen(SCREEN_UV)); over transparents it
// adds only the perturbation. Negative delta components ride the RGBA16F
// scene target (the terminal encode clamps, so a below-black sum clips —
// bounded residual D-PTL-5).
// [orig: CParticleTexture_InitTextureAndChannels @ 0x5e8527/0x5e856b;
// CParticleManager_BuildTextureAtlases @ 0x5e8db0]
render_mode unshaded, cull_disabled, blend_premul_alpha, depth_draw_never, fog_disabled;

uniform sampler2D distort_tex : filter_linear, repeat_disable;
uniform sampler2D screen_tex : hint_screen_texture, filter_linear, repeat_disable;
uniform bool has_texture = false;
uniform bool billboard = true;

void vertex() {
	if (billboard) {
		// The BILLBOARD_PARTICLES device path the pooled MultiMesh presenter
		// relies on: face the view, spin by the instance roll
		// (INSTANCE_CUSTOM.x), keep the instance scale.
		mat4 mat_world = mat4(normalize(INV_VIEW_MATRIX[0]),
				normalize(INV_VIEW_MATRIX[1]),
				normalize(INV_VIEW_MATRIX[2]), MODEL_MATRIX[3]);
		mat_world = mat_world * mat4(
				vec4(cos(INSTANCE_CUSTOM.x), -sin(INSTANCE_CUSTOM.x), 0.0, 0.0),
				vec4(sin(INSTANCE_CUSTOM.x), cos(INSTANCE_CUSTOM.x), 0.0, 0.0),
				vec4(0.0, 0.0, 1.0, 0.0),
				vec4(0.0, 0.0, 0.0, 1.0));
		mat_world = mat_world * mat4(
				vec4(length(MODEL_MATRIX[0].xyz), 0.0, 0.0, 0.0),
				vec4(0.0, length(MODEL_MATRIX[1].xyz), 0.0, 0.0),
				vec4(0.0, 0.0, length(MODEL_MATRIX[2].xyz), 0.0),
				vec4(0.0, 0.0, 0.0, 1.0));
		MODELVIEW_MATRIX = VIEW_MATRIX * mat_world;
		MODELVIEW_NORMAL_MATRIX = mat3(MODELVIEW_MATRIX);
	}
}

void fragment() {
	vec4 texel = vec4(0.5, 0.5, 1.0, 1.0);
	if (has_texture) {
		texel = texture(distort_tex, UV);
	} else {
		vec2 centered = UV - vec2(0.5);
		float r = length(centered) * 2.0;
		texel.a = clamp(1.0 - r, 0.0, 1.0);
	}
	vec3 normal = texel.rgb * 2.0 - 1.0;
	// GetTickCount() * 0.004 -> seconds * 4; the wave triple is 1/51.2.
	float theta = TIME * 4.0;
	float wave_x = sin(theta) * 0.01953125;
	float wave_y = cos(theta) * 0.01953125;
	float wave_z = -sin(theta) * 0.01953125;
	float alpha = COLOR.a;
	vec2 scene_uv;
	scene_uv.x = normal.x * (alpha * wave_x) + normal.y * (alpha * wave_y)
			+ normal.z * SCREEN_UV.x;
	scene_uv.y = normal.x * (alpha * wave_y) + normal.y * (alpha * wave_z)
			+ normal.z * SCREEN_UV.y;
	vec3 delta = texture(screen_tex, scene_uv).rgb
			- texture(screen_tex, SCREEN_UV).rgb;
	ALBEDO = delta * (alpha * texel.a);
	ALPHA = 0.0;
}
