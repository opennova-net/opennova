shader_type spatial;
render_mode blend_mul, depth_draw_never, cull_back, unshaded, fog_disabled;

// The render-slot entity ground-shadow drape, as a multiplicative next pass
// over the hand-lit terrain surface. Retail drapes each bound shadow slot
// over a 21x21 terrain-following patch: the per-entity silhouette RT is
// projected along the slot light direction and multiplied into the terrain
// with the per-channel ambient law and the 40..80 u distance fade — or, for
// a bound slot without a silhouette RT, the authored items.def `shadow` blob
// decal, heading-rotated (retail: RenderSlot_DrawAllDrapes @0x5d6e20,
// render_sector_model @0x5d5ca0, blob @0x5d59d0 — see
// docs/render/render-lighting-re.md). The terrain surface itself stands in
// for the patch mesh; the projection is evaluated per pixel.
//
// Per slot i:
//   u_slot_mat_i : world -> (u, v, depth01) projector matrix
//   u_slot_term[i] : rgb = the per-channel shadow term
//                    q_c = sun_c*|dir_y| / (sun_c*|dir_y| + sky_c)
//                    (retail: render_sector_model @0x5d5f63..0x5d6008);
//                    a = 0 off, 1 silhouette, 2 authored blob
// Shadowed terrain = terrain * (1 - (1-fade) * q * mask) — exactly retail's
// multiply blend of (silhouette + ambient-law material) over the patch.

const int MAX_SLOTS = 12;
// The drape distance fade (start, range) in world units; SlotShadow pushes
// the pair from renderer::kDrapeFadeStartUnits/kDrapeFadeEndUnits.
uniform vec2 u_drape_fade_range = vec2(40.0, 40.0);
uniform vec4 u_slot_term[12];
uniform sampler2D u_slot_tex_0 : source_color, filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_1 : source_color, filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_2 : source_color, filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_3 : source_color, filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_4 : source_color, filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_5 : source_color, filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_6 : source_color, filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_7 : source_color, filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_8 : source_color, filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_9 : source_color, filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_10 : source_color, filter_linear, repeat_disable;
uniform sampler2D u_slot_tex_11 : source_color, filter_linear, repeat_disable;
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

void vertex() {
	v_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
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
	// The drape distance fade (renderer::drape_fade: 0 inside the start
	// distance, ramping to 1 over the range).
	float fade = clamp((length(VERTEX) - u_drape_fade_range.x) /
			u_drape_fade_range.y, 0.0, 1.0);
	vec3 factor = vec3(1.0);
	for (int i = 0; i < MAX_SLOTS; i++) {
		float mode = u_slot_term[i].a;
		if (mode < 0.5) {
			continue;
		}
		vec4 p = slot_matrix(i) * vec4(v_world, 1.0);
		if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0 ||
				p.z <= 0.0 || p.z >= 1.0) {
			continue;
		}
		vec4 s = slot_sample(i, p.xy);
		if (mode < 1.5) {
			// Silhouette: mask = capture alpha; the multiply removes the
			// faded sun term per channel.
			factor *= vec3(1.0) - (1.0 - fade) * u_slot_term[i].rgb * s.a;
		} else {
			// Authored blob decal: retail multiplies the terrain by the
			// decal texture directly, no distance fade on this leg
			// (retail: @0x5d59d0 — black-ambient material, src = tex).
			factor *= mix(vec3(1.0), s.rgb, s.a);
		}
	}
	// Retail's drape multiplies the 8-bit sRGB framebuffer directly (the
	// D3D8-era fixed pipeline has no linear stage); Godot's blend_mul runs
	// in LINEAR space, so the composed sRGB factor folds through the
	// transfer curve to darken the displayed value by the same law.
	ALBEDO = pow(factor, vec3(2.2));
}
