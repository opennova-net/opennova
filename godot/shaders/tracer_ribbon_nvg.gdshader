shader_type spatial;
// The NVG laser ribbon, pool+0x3008 (retail
// CEffectEmitterPool_CreateShaders @ 0x5DC8F0): smoktest.pcx on both
// stages, SRCALPHA/ONE; colour = DIFFUSE, alpha = DIFFUSE.a x (1 - T0.a) x
// (1 - T1.a), the second texture on the second coordinate set; the style's
// +0 word fogs to black (SetFogAndBlendMode mode 2, retail
// CEffectChannel_RenderRibbon @ 0x5DC85A). The pass flags 0x10520000:
// lighting off, cull none, z-write off, fog on.
render_mode unshaded, cull_disabled, blend_add, depth_draw_never;

#include "res://shaders/color.gdshaderinc"
#include "res://shaders/effect_fog.gdshaderinc"

uniform sampler2D smoke_tex : repeat_enable, filter_linear_mipmap;
uniform bool fog_black = true;

varying float v_eye_depth;
varying float v_radial_distance;

void vertex() {
	vec3 view_pos = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_eye_depth = -view_pos.z;
	v_radial_distance = length(view_pos);
}

void fragment() {
	float alpha = COLOR.a * (1.0 - texture(smoke_tex, UV).a) *
			(1.0 - texture(smoke_tex, UV2).a);
	float visibility = effect_fog_visibility(v_eye_depth, v_radial_distance);
	vec3 rgb = fog_black ? effect_fog_black(COLOR.rgb, visibility)
			: effect_fog_scene(COLOR.rgb, visibility);
	ALBEDO = scene_output(rgb);
	ALPHA = alpha;
}
