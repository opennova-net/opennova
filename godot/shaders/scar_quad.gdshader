shader_type spatial;
// The impact-scar quad in the SCORCH drawer state — GfxShader mode word
// 0x120651, the state every strip but bhole1 selects [orig: Scar_LoadTextures
// @0x5CC315 -> GfxShader_Create1TexModeId @0x679030; the drawer
// Scar_DrawBatches @0x5CCD10 (ex Terrain_RenderFoliageBatches, the kong
// misnomer, renamed 2026-08-25) under Scar_RenderAllCaches @0x5CDF70; the decode
// renderer::decode_scar_strip_mode, docs/world/world-wac-ai-re.md §24.9]:
//   SRCALPHA / INVSRCALPHA framebuffer blend      -> blend_mix
//   LIGHTING off (pass flag 0x10000000)            -> unshaded
//   FOGENABLE (0x20000)                            -> the pass fog toward the
//       scene fog colour (the drawer's CD3DDevice_SetFogAndBlendMode(dev, 0)
//       @ 0x5CCD33), effect_fog.gdshaderinc; alpha is not fogged
//   z-write OFF (0x100000), z-test on              -> depth_draw_never
//   CULLMODE CCW (no 0x400000): back faces culled  -> cull_back (the packer
//       winds every quad so its front is the struck face's normal side,
//       simulation_scars.cpp)
//   NO ALPHATESTENABLE (no 0x40000): the drawer's SetAlphaTestRef(128)
//       @0x5CCDAE latches ALPHAFUNC/ALPHAREF only, so it is INERT here
//   stage 0: COLOROP MODULATE2X(TEXTURE, DIFFUSE), ALPHAOP MODULATE(TEXTURE,
//       DIFFUSE); DIFFUSE = g_EnvTerrainLightCombined | FF000000 on every
//       vertex; the strip's TGA with clamp wrap.
// The scorch TGAs are black RGB under an alpha falloff (max ~0.75): the mark
// IS the blend. An alpha scissor here (the 2026-08-21 first cut) dropped the
// blend and painted an opaque black blob.
render_mode unshaded, blend_mix, depth_draw_never, cull_back;

// Gamma-space pipeline (D-RMAT-7): raw texel sampling, gamma-space combine,
// the exact inverse of the blit's transfer curve on the way out.
#include "res://shaders/color.gdshaderinc"
#include "res://shaders/effect_fog.gdshaderinc"

uniform sampler2D albedo_tex : repeat_disable, filter_linear_mipmap;

// The presenter's ONE device fold: retail writes no depth bias — the scar quad
// is coplanar with the struck face and the D3D rasteriser's own bias keeps it
// on top. Godot has no polygon offset, so the quad is pulled 0.2% toward the
// eye in view space (convention-independent: works under forward or reverse
// Z) — a displacement of 2 mm at 1 m, 20 cm at 100 m, well under the fog cull.
const float SCAR_VIEW_PULL = 0.998;

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
	// MODULATE2X(TEXTURE, DIFFUSE), saturating [orig: RenderState_DecodeModeColorStage
	// @0x681080 family 0x600 under GfxDevice_Modulate2XEnabled].
	vec3 rgb = min(tex.rgb * COLOR.rgb * 2.0, vec3(1.0));
	ALBEDO = scene_output(effect_fog_scene(rgb,
			effect_fog_visibility(v_eye_depth, v_radial_distance)));
	// MODULATE(TEXTURE, DIFFUSE) alpha [orig: RenderState_DecodeModeAlphaStage @0x680B00
	// nibble 0x50]; DIFFUSE alpha is the vertex's FF.
	ALPHA = tex.a * COLOR.a;
}
