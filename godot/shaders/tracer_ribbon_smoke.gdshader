shader_type spatial;
// The rocket/at4/grenade smoke ribbon, pool+0x3004 (retail
// CEffectEmitterPool_CreateShaders @ 0x5DC8F0): smoktest.pcx on both
// stages, SRCALPHA/INVSRCALPHA; stage 0 colour = DIFFUSE, alpha =
// SUBTRACT(DIFFUSE, TEXTURE); stage 1 colour = CURRENT, alpha =
// SUBTRACT(CURRENT, TEXTURE) on the second coordinate set; the scene fog
// colour (SetFogAndBlendMode mode 0, retail CEffectChannel_RenderRibbon
// @ 0x5DC85E). The pass flags 0x10520000: lighting off, cull none, z-write
// off, fog on. The texture's alpha is its palette luminance (retail
// Texture_LoadFromArchive @ 0x58B980), mipmapped (GTexture_CreateFromPixelData_0
// @ 0x6876C0) with wrap addressing.
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never;

#include "res://shaders/color.gdshaderinc"
#include "res://shaders/effect_fog.gdshaderinc"

uniform sampler2D smoke_tex : repeat_enable, filter_linear_mipmap;
uniform bool fog_black = false;

varying float v_eye_depth;
varying float v_radial_distance;

void vertex() {
	vec3 view_pos = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_eye_depth = -view_pos.z;
	v_radial_distance = length(view_pos);
}

void fragment() {
	float stage0 = clamp(COLOR.a - texture(smoke_tex, UV).a, 0.0, 1.0);
	float stage1 = clamp(stage0 - texture(smoke_tex, UV2).a, 0.0, 1.0);
	float visibility = effect_fog_visibility(v_eye_depth, v_radial_distance);
	vec3 rgb = fog_black ? effect_fog_black(COLOR.rgb, visibility)
			: effect_fog_scene(COLOR.rgb, visibility);
	ALBEDO = scene_output(rgb);
	ALPHA = stage1;
}
