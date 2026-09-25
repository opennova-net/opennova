shader_type spatial;
// The impact-scar quad in the BULLET-HOLE drawer state — GfxShader mode word
// 0x460651, the state the bhole1 strip (index 27, modeId 1) selects
// [orig: Scar_LoadTextures @0x5CC321 -> GfxShader_Create1TexModeId @0x679030;
// the drawer Scar_DrawBatches @0x5CCD10 (ex Terrain_RenderFoliageBatches,
// renamed 2026-08-25); renderer::decode_scar_strip_mode, docs/world/world-wac-ai-re.md
// §24.9]. The scorch word's state (scar_quad.gdshader) plus:
//   ALPHATESTENABLE (0x40000): the drawer's SetAlphaTestRef(128) @0x5CCDAE
//       latch is LIVE here — ALPHAFUNC GREATER, ALPHAREF 128  -> the discard
//   z-write ON (no 0x100000)                      -> depth_draw_always
//   CULLMODE NONE (0x400000)                      -> cull_disabled
//   FOGENABLE (0x20000), shared with the scorch word -> the pass fog toward
//       the scene fog colour, effect_fog.gdshaderinc
render_mode unshaded, blend_mix, depth_draw_always, cull_disabled;

// Gamma-space pipeline (D-RMAT-7): raw texel sampling, gamma-space combine,
// the exact inverse of the blit's transfer curve on the way out.
#include "res://shaders/color.gdshaderinc"
#include "res://shaders/effect_fog.gdshaderinc"

uniform sampler2D albedo_tex : repeat_disable, filter_linear_mipmap;

// The same view-space pull as the scorch quad (see scar_quad.gdshader).
const float SCAR_VIEW_PULL = 0.998;

// D3DRS_ALPHAREF 128 under ALPHAFUNC GREATER, on the 8-bit stage output.
const float SCAR_ALPHA_TEST_REF = 128.0;

varying float v_eye_depth;
varying float v_radial_distance;

void vertex() {
	vec4 view_pos = MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
	v_eye_depth = -view_pos.z;
	v_radial_distance = length(view_pos.xyz);
	view_pos.xyz *= SCAR_VIEW_PULL;
	POSITION = PROJECTION_MATRIX * view_pos;
}

void fragment() {
	vec4 tex = texture(albedo_tex, UV);
	// MODULATE(TEXTURE, DIFFUSE) alpha feeds both the test and the blend.
	float alpha = tex.a * COLOR.a;
	if (alpha * 255.0 <= SCAR_ALPHA_TEST_REF) {
		discard;
	}
	// MODULATE2X(TEXTURE, DIFFUSE), saturating.
	vec3 rgb = min(tex.rgb * COLOR.rgb * 2.0, vec3(1.0));
	ALBEDO = scene_output(effect_fog_scene(rgb,
			effect_fog_visibility(v_eye_depth, v_radial_distance)));
	ALPHA = alpha;
}
