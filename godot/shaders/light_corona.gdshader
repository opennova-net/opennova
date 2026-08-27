shader_type spatial;
// EffectWorld light corona billboards (D-RLIT-4): additive camera-facing
// quads at each pool light, three shrinking segments marching toward the
// camera. Segment centers/half-sizes/colors are computed by the portable
// walk (renderer::LightScene::collect_corona_quads — witness map there
// [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40, fog+blend mode 2
// additive, depth-tested, no depth write]); the MultiMesh instance carries
// center (origin) + half_size (uniform basis scale), the instance COLOR the
// premultiplied additive color — the segment fade, the owner
// visible-section gate, and the fog-to-black fold (mode 2 forces FOGCOLOR
// black [orig: @ 0x677740 case 2]) are already folded in by the collect.
render_mode unshaded, depth_draw_never, cull_disabled, blend_add;

// Gamma-domain scene-target contract.
#include "res://shaders/color.gdshaderinc"
#include "res://shaders/frame_pass.gdshaderinc"

// The procedural 128x128 radial falloff "texlightcrn" [orig:
// Lighting_InitTextures @ 0x5a94f0 — intensity = 255 x (0.4 - 0.45 x d),
// d = sqrt(((x-64)/64)^2 + ((y-64)/64)^2), border texels forced 0].
uniform sampler2D u_corona_tex : filter_linear, repeat_disable;

void vertex() {
	// Camera-facing rebuild per pass (main and mirror views each get a true
	// billboard), half_size from the instance's uniform scale.
	vec3 center = MODEL_MATRIX[3].xyz;
	float half_size = length(MODEL_MATRIX[0].xyz);
	vec3 pass_right = normalize(INV_VIEW_MATRIX[0].xyz);
	vec3 pass_up = normalize(INV_VIEW_MATRIX[1].xyz);
	vec3 world_pos = center
			+ pass_right * VERTEX.x * half_size
			+ pass_up * VERTEX.y * half_size;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(world_pos, 1.0);
}

void fragment() {
	if (is_q3_pass(CAMERA_VISIBLE_LAYERS, IN_SHADOW_PASS)) {
		discard;
	}
	// Gamma-space modulate and accumulation before the terminal transfer.
	ALBEDO = scene_output(texture(u_corona_tex, UV).rgb * COLOR.rgb);
	ALPHA = 1.0;
}
