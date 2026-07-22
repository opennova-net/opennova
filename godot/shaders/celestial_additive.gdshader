shader_type spatial;
// Additive sky bodies (glare, stars). The engine loads glare/star models
// additively (mode 0x300000 [orig: EffectWorld_LoadCelestialModels @ 0x5adc50]);
// they write light into the framebuffer rather than occluding. Depth-tested
// (no write) like the plain bodies. Brightness scales by u_opacity - the
// glare's is the witnessed dot^4/2 x occlusion chain
// [orig: render_skybox_sun_glow @ 0x5acd00]. docs/env/env-tod-re.md.
render_mode unshaded, depth_draw_never, cull_disabled, blend_add;

// Gamma-space output convention (D-RMAT-7): nova_gamma_to_linear().
#include "res://shaders/nova_color.gdshaderinc"

// Raw sampling per the gamma-space convention (nova_color.gdshaderinc).
uniform sampler2D u_diffuse : filter_linear_mipmap;
uniform vec3 u_tint = vec3(1.0, 1.0, 1.0);
uniform float u_opacity = 1.0;
uniform vec3 u_anchor_camera_world = vec3(0.0);
uniform bool u_billboard = false;
uniform bool u_glare_view_fade = false;
uniform vec3 u_glare_direction = vec3(0.0, 1.0, 0.0);

varying float v_glare_view_fade;

void vertex() {
	vec3 pass_eye = INV_VIEW_MATRIX[3].xyz;
	vec3 anchor_delta = pass_eye - u_anchor_camera_world;
	vec3 world_pos;
	if (u_billboard) {
		// MultiMesh instance transforms carry only local offset + scale. Rebuild
		// the quad with THIS pass camera's basis so main and mirror views each
		// receive a true camera-facing star field.
		vec3 center = MODEL_MATRIX[3].xyz + anchor_delta;
		vec3 pass_right = normalize(INV_VIEW_MATRIX[0].xyz);
		vec3 pass_up = normalize(INV_VIEW_MATRIX[1].xyz);
		vec3 pass_back = normalize(INV_VIEW_MATRIX[2].xyz);
		vec3 instance_scale = vec3(
				length(MODEL_MATRIX[0].xyz),
				length(MODEL_MATRIX[1].xyz),
				length(MODEL_MATRIX[2].xyz));
		world_pos = center
				+ pass_right * VERTEX.x * instance_scale.x
				+ pass_up * VERTEX.y * instance_scale.y
				+ pass_back * VERTEX.z * instance_scale.z;
	} else {
		world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz + anchor_delta;
	}
	vec3 pass_forward = normalize(-INV_VIEW_MATRIX[2].xyz);
	float view_dot = max(dot(pass_forward, normalize(u_glare_direction)), 0.0);
	float view_dot_sq = view_dot * view_dot;
	v_glare_view_fade = view_dot_sq * view_dot_sq;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(world_pos, 1.0);
}

void fragment() {
	vec4 tex = texture(u_diffuse, UV);
	float opacity = u_opacity * (u_glare_view_fade ? v_glare_view_fade : 1.0);
	// Gamma-space modulate + encoded output (D-RMAT-7); additive accumulation
	// happens in the linear buffer — the bounded residual is D-RMAT-8.
	// COLOR = per-instance brightness for the star field's twinkle (env #33;
	// plain meshes default to white).
	ALBEDO = nova_gamma_to_linear(tex.rgb * u_tint * COLOR.rgb * tex.a * opacity);
	ALPHA = 1.0;
}
