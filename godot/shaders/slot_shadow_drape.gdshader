shader_type spatial;
render_mode blend_mul, depth_draw_never, cull_disabled, unshaded, fog_disabled;

// The terrain's fog uniforms and law: the drape shares the device's primary
// fog config with the terrain batch it multiplies into (SlotShadow pushes the
// same values through MissionEnvironment.apply_terrain_uniforms).
#include "res://shaders/terrain_lighting.gdshaderinc"
#include "res://shaders/color.gdshaderinc"

// The render-slot entity ground-shadow drape: one terrain-following patch
// mesh per slot that owns a silhouette RT, (lod + 1)^2 vertices at 1 u over
// the point-sampled terrain heights plus the (lod + 1) * 0.004 u lift, drawn
// with culling off, no depth writes and the multiply blend right after the
// terrain (retail: RenderSlot_DrawAllDrapes @0x5d6e20,
// RenderSlot_DrawSilhouetteDrape @0x5d5ca0, RenderSlot_RebuildPatchVertexBuffer
// @0x5d5130; the technique's intrinsic flags 0x1520000 @0x5d62f7 — see
// docs/render/render-lighting-re.md). SlotShadow builds the meshes
// (renderer::slot_patch_vertices / slot_patch_indices); every instance shares
// this material and names its slot in the instance uniform u_slot. A bound
// slot without an RT draws nothing: its authored-blob leg (@0x5d59d0) is dead
// in JO.
//
// Per slot i:
//   u_slot_mat_i : world -> (u, v) projector matrix (the stage-0 texgen)
//   u_slot_term[i] : rgb = the slot's fixed-function material ambient
//                    1 - (1 - fade) * q_c, q_c = sun_c*|dir_y| /
//                    (sun_c*|dir_y| + sky_c), with the fade taken ONCE per
//                    slot from the entity-to-camera distance (retail:
//                    RenderSlot_DrawSilhouetteDrape @0x5d5cc4..0x5d5d59 and
//                    @0x5d5f63..0x5d6008; renderer::drape_sun_ambient);
//                    a = 0 off (no RT, or culled at >= 80 u), 1 silhouette;
//                    white for an attached-light slot, whose term is the
//                    light below
//   u_slot_light_pos[i], u_slot_light_diffuse[i] : an attached-light slot's
//                    D3D light 4 as (position, range) and (rescaled
//                    negative diffuse, quadratic attenuation); range 0 = the
//                    sun leg. Each patch vertex takes sat(1 + diffuse * atten
//                    * N.L) on its (0, 1, 0) normal, atten = 1 / (1 + q d^2)
//                    inside the range and 0 past it, Gouraud-interpolated
//                    (retail: RenderSlot_DrawSilhouetteDrape @0x5d5e50..0x5d5f4c;
//                    renderer::drape_attached_light_color)
//   u_slot_clip_u[i], u_slot_clip_v[i] : the depth-clip stage's texgen as
//                    world dot products (xyz . p + w) into u_shadowztex —
//                    retail's second texture stage ADDs that 32x4 texture to
//                    the silhouette term, so its white half (toward the light
//                    / in front of the caster plane) and its far row suppress
//                    the shadow and its black half keeps it (retail:
//                    build_shadow_cascade_uv_matrices @0x58cf10,
//                    shadow_system_init_resources @0x5d6260;
//                    renderer::slot_depth_clip, shadowztex_pixels)
// Both texgens run per patch vertex like retail's texture-transformed
// camera-space coordinates. Shadowed terrain = terrain * fog(sat(sat(rt +
// colour) + ztex)): retail's two-stage ADD under the multiply blend, fogged
// toward WHITE by the primary fog config (retail: RenderSlot_DrawAllDrapes
// @0x5d6ea1, CD3DDevice_SetFogAndBlendMode mode 3). Both samplers clamp
// (the technique's stage-0 clamp; shadowztex created clamp @0x5d62a9).

instance uniform int u_slot = 0;
uniform vec4 u_slot_term[12];
uniform vec4 u_slot_clip_u[12];
uniform vec4 u_slot_clip_v[12];
uniform vec4 u_slot_light_pos[12];
uniform vec4 u_slot_light_diffuse[12];
// The 32x4 "shadowztex" step, CLAMP + bilinear like retail's sampler.
uniform sampler2D u_shadowztex : filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_0 : filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_1 : filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_2 : filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_3 : filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_4 : filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_5 : filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_6 : filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_7 : filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_8 : filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_9 : filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_10 : filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_11 : filter_linear, repeat_disable;
uniform mat4 u_slot_mat_0;
uniform mat4 u_slot_mat_1;
uniform mat4 u_slot_mat_2;
uniform mat4 u_slot_mat_3;
uniform mat4 u_slot_mat_4;
uniform mat4 u_slot_mat_5;
uniform mat4 u_slot_mat_6;
uniform mat4 u_slot_mat_7;
uniform mat4 u_slot_mat_8;
uniform mat4 u_slot_mat_9;
uniform mat4 u_slot_mat_10;
uniform mat4 u_slot_mat_11;

varying vec3 v_world;
varying float v_eye_depth;
varying vec2 v_uv;
varying vec2 v_zuv;
varying vec3 v_colour;

vec4 slot_sample(int i, vec2 uv) {
	switch (i) {
		case 0: return texture(u_slot_tex_0, uv);
		case 1: return texture(u_slot_tex_1, uv);
		case 2: return texture(u_slot_tex_2, uv);
		case 3: return texture(u_slot_tex_3, uv);
		case 4: return texture(u_slot_tex_4, uv);
		case 5: return texture(u_slot_tex_5, uv);
		case 6: return texture(u_slot_tex_6, uv);
		case 7: return texture(u_slot_tex_7, uv);
		case 8: return texture(u_slot_tex_8, uv);
		case 9: return texture(u_slot_tex_9, uv);
		case 10: return texture(u_slot_tex_10, uv);
		default: return texture(u_slot_tex_11, uv);
	}
}

mat4 slot_matrix(int i) {
	switch (i) {
		case 0: return u_slot_mat_0;
		case 1: return u_slot_mat_1;
		case 2: return u_slot_mat_2;
		case 3: return u_slot_mat_3;
		case 4: return u_slot_mat_4;
		case 5: return u_slot_mat_5;
		case 6: return u_slot_mat_6;
		case 7: return u_slot_mat_7;
		case 8: return u_slot_mat_8;
		case 9: return u_slot_mat_9;
		case 10: return u_slot_mat_10;
		default: return u_slot_mat_11;
	}
}

// An attached-light slot's fixed-function colour at one patch vertex
// (renderer::drape_attached_light_color).
vec3 attached_light_color(int i, vec3 vertex_world) {
	vec4 light = u_slot_light_pos[i];
	vec3 to_light = light.xyz - vertex_world;
	float d = length(to_light);
	if (d <= 0.0 || d > light.w) {
		return vec3(1.0);
	}
	float attenuation = 1.0 / (1.0 + u_slot_light_diffuse[i].w * d * d);
	float n_dot_l = max(0.0, to_light.y / d);
	return clamp(vec3(1.0) + u_slot_light_diffuse[i].rgb * (attenuation * n_dot_l),
			vec3(0.0), vec3(1.0));
}

void vertex() {
	int i = clamp(u_slot, 0, 11);
	v_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_eye_depth = max(-(MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).z, 0.0);
	v_uv = (slot_matrix(i) * vec4(v_world, 1.0)).xy;
	v_zuv = vec2(dot(u_slot_clip_u[i].xyz, v_world) + u_slot_clip_u[i].w,
			dot(u_slot_clip_v[i].xyz, v_world) + u_slot_clip_v[i].w);
	v_colour = u_slot_light_pos[i].w > 0.0
			? attached_light_color(i, v_world)
			: u_slot_term[i].rgb;
}

void fragment() {
	int i = clamp(u_slot, 0, 11);
	if (u_slot_term[i].a < 0.5) {
		discard;
	}
	// The drape is a fogged multiply pass toward WHITE. The terrain's
	// apply_terrain_fog is the linear mix(fog colour, colour, visibility), so
	// its white-minus-black response is the visibility itself.
	float visibility = apply_terrain_fog(vec3(1.0), v_world, CAMERA_POSITION_WORLD,
			v_eye_depth).r - apply_terrain_fog(vec3(0.0), v_world,
			CAMERA_POSITION_WORLD, v_eye_depth).r;
	// Retail stage 0 ADDs the black-on-white slot RT RGB to the vertex colour;
	// stage 1 ADDs shadowztex, and both stages saturate. Keeping the
	// resolved/filter RGB is significant: an alpha-product approximation
	// leaves faint partial-coverage tails at grazing sun angles
	// (RenderSlot_DrawSilhouetteDrape @0x5d5ca0).
	vec4 s = slot_sample(i, v_uv);
	float ztex = texture(u_shadowztex, v_zuv).r;
	vec3 shadowed = min(vec3(1.0), s.rgb + v_colour);
	vec3 slot_factor = min(vec3(1.0), shadowed + vec3(ztex));
	// Retail's drape multiplies gamma-space framebuffer bytes directly. Both
	// the sampled slot textures and scene target stay in that domain here.
	ALBEDO = scene_output(mix(vec3(1.0), slot_factor, visibility));
}
