// The scene pass device fog for the effect draws that ride it — the tracer
// ribbons and the impact scars. One fog state fogs every draw of the pass
// (device_fog.gdshaderinc, the engine's law): the pass start is already
// Render_SetFogState's per-type start with the overcast fold (type 2 =
// (1 - density) * end * 0.5, type 3 = (1 - density) * end * 0.25; retail
// Render_SetFogState @ 0x58a950 -> CD3DDevice_SetFogParameters @ 0x677960).
// The colour a draw fogs toward is its own SetFogAndBlendMode choice (retail
// CD3DDevice_SetFogAndBlendMode @ 0x677740: mode 0 the scene fog colour,
// mode 2 black), so callers pick it. Alpha is never fogged.
#include "res://shaders/device_fog.gdshaderinc"

global uniform bool opennova_fog_enabled;
global uniform vec3 opennova_fog_color;
global uniform float opennova_fog_start;
global uniform float opennova_fog_end;
global uniform int opennova_fog_type;

// 1.0 = unfogged.
float effect_fog_visibility(float eye_depth, float radial_distance) {
	return device_fog_visibility(eye_depth, radial_distance, opennova_fog_start,
			opennova_fog_end, opennova_fog_type, opennova_fog_enabled);
}

// Toward the scene fog colour (SetFogAndBlendMode mode 0).
vec3 effect_fog_scene(vec3 rgb, float visibility) {
	return mix(opennova_fog_color, rgb, visibility);
}

// Toward black (SetFogAndBlendMode mode 2): the additive families fade out.
vec3 effect_fog_black(vec3 rgb, float visibility) {
	return rgb * visibility;
}
