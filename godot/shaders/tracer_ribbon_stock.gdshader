shader_type spatial;
// The tracer ribbon on the device's stock slot 6 (mode word 0x222): ONE/ONE,
// colour = alpha = DIFFUSE, untextured — the std/rapid/sniper/df1 styles
// (retail CEffectChannel_Init @ 0x5DB1D6, the stock block built by sub_6780A0
// @ 0x678113). The pass flags 0x10520000 (retail CEffectChannel_RenderRibbon
// @ 0x5DC86C): lighting off, cull none, z-write off, fog on; the +0 style
// word fogs to black (SetFogAndBlendMode mode 2 @ 0x5DC85A). The geometry is
// built natively (engine/runtime/renderer/tracer_frame.cpp).
render_mode unshaded, cull_disabled, blend_add, depth_draw_never;

#include "res://shaders/color.gdshaderinc"
#include "res://shaders/effect_fog.gdshaderinc"

uniform bool fog_black = true;

varying float v_eye_depth;
varying float v_radial_distance;

void vertex() {
	vec3 view_pos = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_eye_depth = -view_pos.z;
	v_radial_distance = length(view_pos);
}

void fragment() {
	float visibility = effect_fog_visibility(v_eye_depth, v_radial_distance);
	vec3 rgb = fog_black ? effect_fog_black(COLOR.rgb, visibility)
			: effect_fog_scene(COLOR.rgb, visibility);
	ALBEDO = scene_output(rgb);
	// ONE/ONE: Godot's add is SRCALPHA/ONE, so alpha rides at 1.
	ALPHA = 1.0;
}
