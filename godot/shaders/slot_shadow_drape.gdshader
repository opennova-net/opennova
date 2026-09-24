shader_type spatial;
render_mode blend_mul, depth_draw_never, cull_back, unshaded, fog_disabled;

// The terrain's fog uniforms and law: the drape shares the device's primary
// fog config with the terrain batch it multiplies into (SlotShadow pushes the
// same values through MissionEnvironment.apply_terrain_uniforms).
#include "res://shaders/terrain_lighting.gdshaderinc"
#include "res://shaders/color.gdshaderinc"

// The render-slot entity ground-shadow drape, as a multiplicative next pass
// over the hand-lit terrain surface. Retail drapes each bound shadow slot
// that owns a silhouette RT over a lod x lod terrain-following patch: the
// per-entity silhouette RT is projected along the slot light direction and
// multiplied into the terrain with the per-channel ambient law and the
// 40..80 u distance fade (retail: RenderSlot_DrawAllDrapes @0x5d6e20,
// RenderSlot_DrawSilhouetteDrape @0x5d5ca0 — see
// docs/render/render-lighting-re.md). A bound slot without an RT draws
// nothing: its authored-blob leg (@0x5d59d0) is dead in JO. The terrain
// surface itself stands in for the patch mesh; the projection is evaluated
// per pixel.
//
// Per slot i:
//   u_slot_mat_i : world -> (u, v) projector matrix (the stage-0 texgen)
//   u_slot_term[i] : rgb = the slot's fixed-function material ambient
//                    1 - (1 - fade) * q_c, q_c = sun_c*|dir_y| /
//                    (sun_c*|dir_y| + sky_c), with the fade taken ONCE per
//                    slot from the entity-to-camera distance (retail:
//                    RenderSlot_DrawSilhouetteDrape @0x5d5cc4..0x5d5d59 and
//                    @0x5d5f63..0x5d6008; renderer::drape_sun_ambient);
//                    a = 0 off (no RT, or culled at >= 80 u), 1 silhouette
//   u_slot_patch[i] : the lod x lod terrain patch the slot is draped over,
//                    as world (min_x, min_z, max_x, max_z) — retail builds a
//                    (lod+1)^2 terrain-following mesh of side lod = the
//                    grazing-scaled clamp(2r+1, 6, 20) u around the marched
//                    anchor and draws nothing outside it (retail:
//                    RenderSlot_RebuildPatchVertexBuffer @0x5d5130;
//                    renderer::slot_patch_bounds)
//   u_slot_clip_u[i], u_slot_clip_v[i] : the depth-clip stage's texgen as
//                    world dot products (xyz . p + w) into u_shadowztex —
//                    retail's second texture stage ADDs that 32x4 texture to
//                    the silhouette term, so its white half (toward the light
//                    / in front of the caster plane) and its far row suppress
//                    the shadow and its black half keeps it (retail:
//                    build_shadow_cascade_uv_matrices @0x58cf10,
//                    shadow_system_init_resources @0x5d6260;
//                    renderer::slot_depth_clip, shadowztex_pixels)
// Shadowed terrain = terrain * fog(sat(sat(rt + ambient) + ztex)) inside the
// patch — exactly retail's two-stage ADD under the multiply blend, the result
// fogged toward WHITE by the primary fog config (retail: RenderSlot_Draw-
// AllDrapes @0x5d6ea1, CD3DDevice_SetFogAndBlendMode mode 3). Both samplers
// clamp (retail: the drape technique's stage-0 clamp, intrinsic flags
// 0x1520000 @0x5d62f7; shadowztex created clamp @0x5d62a9): the projection is
// unbounded, the RT edge texels (the white clear) cover the rest of the patch.

const int MAX_SLOTS = 12;
uniform vec4 u_slot_term[12];
uniform vec4 u_slot_patch[12];
uniform vec4 u_slot_clip_u[12];
uniform vec4 u_slot_clip_v[12];
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

void vertex() {
	v_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_eye_depth = max(-(MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).z, 0.0);
}

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

void fragment() {
	// Each slot is its own fogged multiply pass toward WHITE. The terrain's
	// apply_terrain_fog is the linear mix(fog colour, colour, visibility), so
	// its white-minus-black response is the visibility itself.
	float visibility = apply_terrain_fog(vec3(1.0), v_world, CAMERA_POSITION_WORLD,
			v_eye_depth).r - apply_terrain_fog(vec3(0.0), v_world,
			CAMERA_POSITION_WORLD, v_eye_depth).r;
	vec3 factor = vec3(1.0);
	for (int i = 0; i < MAX_SLOTS; i++) {
		float mode = u_slot_term[i].a;
		if (mode < 0.5) {
			continue;
		}
		// The drape patch: nothing outside the slot's lod x lod square.
		vec4 patch = u_slot_patch[i];
		if (v_world.x < patch.x || v_world.x > patch.z ||
				v_world.z < patch.y || v_world.z > patch.w) {
			continue;
		}
		vec4 p = slot_matrix(i) * vec4(v_world, 1.0);
		vec4 s = slot_sample(i, p.xy);
		// Retail stage 0 ADDs the black-on-white slot RT RGB to the faded
		// ambient material; stage 1 ADDs shadowztex, and both stages
		// saturate. Keeping the resolved/filter RGB is significant: an
		// alpha-product approximation leaves faint partial-coverage tails at
		// grazing sun angles (RenderSlot_DrawSilhouetteDrape @0x5d5ca0).
		vec2 zuv = vec2(dot(u_slot_clip_u[i].xyz, v_world) + u_slot_clip_u[i].w,
				dot(u_slot_clip_v[i].xyz, v_world) + u_slot_clip_v[i].w);
		float ztex = texture(u_shadowztex, zuv).r;
		vec3 shadowed = min(vec3(1.0), s.rgb + u_slot_term[i].rgb);
		vec3 slot_factor = min(vec3(1.0), shadowed + vec3(ztex));
		factor *= mix(vec3(1.0), slot_factor, visibility);
	}
	// Retail's drape multiplies gamma-space framebuffer bytes directly. Both
	// the sampled slot textures and scene target stay in that domain here.
	ALBEDO = scene_output(factor);
}
