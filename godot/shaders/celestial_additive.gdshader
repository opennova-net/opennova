shader_type spatial;
// Additive sky bodies (the glare, glint and the authored-additive discs). The
// engine loads the glare model additively (mode 0x300000 [orig: EffectWorld_LoadCelestialModels @ 0x5adc50]);
// they write light into the framebuffer rather than occluding. Depth-tested
// (no write) like the plain bodies. Brightness scales by u_opacity - the
// glare's is the witnessed dot^4/2 x occlusion chain
// [orig: render_skybox_sun_glow @ 0x5acd00]. docs/env/env-tod-re.md.
render_mode unshaded, depth_draw_never, cull_disabled, blend_add;

// Gamma-domain scene-target contract.
#include "res://shaders/color.gdshaderinc"

// Raw sampling per the gamma-space convention (color.gdshaderinc).
uniform sampler2D u_diffuse : filter_linear_mipmap;
uniform vec3 u_tint = vec3(1.0, 1.0, 1.0);
uniform float u_opacity = 1.0;
uniform vec3 u_anchor_camera_world = vec3(0.0);
// The sun/moon DISC bodies pin to the exact far plane: retail draws them in
// the sky bracket BEFORE the world, so any terrain overdraws them
// [orig: render_celestial_bodies @ 0x5acaa0]. Their authored materials are
// additive (FF_ST_AD_LUM), so the disc renders through THIS shader. The
// glare/glint stay unpinned - retail draws those after the world at the
// 64 u anchor with ray-driven occlusion [orig: render_skybox_sun_glow
// @ 0x5acd00; update_sun_glare @ 0x5ad130].
uniform bool u_depth_far_pin = false;
uniform bool u_glare_view_fade = false;
uniform vec3 u_glare_direction = vec3(0.0, 1.0, 0.0);
// The typed Q3 producer reads this occlusion-independent fog-based opacity;
// the beauty shader itself always uses u_opacity.
uniform float u_q3_opacity = 0.0;
// Which scene passes draw the sun/moon this frame: they ride the sky
// bracket, drawn by the main frame only while blink letter 0x4 is clear and
// the eye is strictly above the water, by the water mirror while letter 0x2
// is clear (renderer/scene_pass_gates.h carries the witnesses). The glare and
// glint keep the defaults: they are drawn outside the bracket.
uniform bool u_beauty_pass_drawn = true;
uniform bool u_mirror_pass_drawn = true;
const uint SKY_WATER_LAYER = 1024u; // visual_layers::WATER (0x00000400)

bool sky_pass_drawn(uint camera_layers) {
	if (camera_layers == SKY_WATER_LAYER) {
		return true; // the environment-capture cameras
	}
	return (camera_layers & SKY_WATER_LAYER) != 0u ? u_beauty_pass_drawn
			: u_mirror_pass_drawn;
}

varying float v_glare_view_fade;

void vertex() {
	vec3 pass_eye = INV_VIEW_MATRIX[3].xyz;
	vec3 anchor_delta = pass_eye - u_anchor_camera_world;
	vec3 world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz + anchor_delta;
	vec3 pass_forward = normalize(-INV_VIEW_MATRIX[2].xyz);
	float view_dot = max(dot(pass_forward, normalize(u_glare_direction)), 0.0);
	float view_dot_sq = view_dot * view_dot;
	v_glare_view_fade = view_dot_sq * view_dot_sq;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(world_pos, 1.0);
	if (u_depth_far_pin) {
		// Exact far plane (reverse-Z depth 0); an epsilon fails because
		// distant terrain's depth near/dist drops below any epsilon. The
		// BEAUTY view only: retail's bloom pass redraws the discs through
		// Render_SetViewportFarDepth's far-band viewport (MinZ 0.98 / MaxZ
		// 0.99996948) and the ordinary z-tested flush, so in the bloom
		// source every nearer surface occludes the disc: a sun behind a
		// ridge does not bloom over it. The focused Q3 adapter applies that
		// band (runtime/renderer/q3_frame.h kQ3FarBandMinZ/MaxZ)
		// [orig: FrameFX_RenderBloomPass @ 0x582940: Render_SetViewportFarDepth
		// @ 0x582a70 -> render_celestial_bodies(1) @ 0x582a77;
		// Render_SetViewportFarDepth @ 0x58a840].
		POSITION.z = 0.0;
	}
	if (!IN_SHADOW_PASS && !sky_pass_drawn(CAMERA_VISIBLE_LAYERS)) {
		POSITION = vec4(2.0, 2.0, 2.0, 1.0);
	}
}

void fragment() {
	vec4 tex = texture(u_diffuse, UV);
	float opacity = u_opacity * (u_glare_view_fade ? v_glare_view_fade : 1.0);
	// Gamma-space modulate; additive accumulation precedes the terminal
	// display transfer and therefore remains in the retail gamma domain.
	// COLOR = the mesh's vertex diffuse (white when the model carries none).
	ALBEDO = scene_output(tex.rgb * u_tint * COLOR.rgb * tex.a * opacity);
	ALPHA = 1.0;
}
