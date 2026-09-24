// Whether retail submits this fragment's MATCHTERRAIN pre-pass at all: a
// skinned crouch/prone draw outside the thermal view with a resident page.
bool obj_match_terrain_active() {
	#ifdef OBJ_MATCHTERRAIN_SKINNED_STANCE
	return !opennova_thermal_view && u_match_terrain_enabled && u_has_match_terrain_cache &&
			u_match_terrain_page_ready;
	#else
	return false;
	#endif
}

// The pre-pass's terrain-page color for this fragment.
vec3 obj_match_terrain_color() {
	#ifdef OBJ_MATCHTERRAIN_SKINNED_STANCE
	vec2 page_uv = (v_world_pos.xz - u_match_terrain_page_projection.xy) *
			u_match_terrain_page_projection.z;
	vec2 half_texel = vec2(0.5 / 256.0);
	page_uv = clamp(page_uv, half_texel, vec2(1.0) - half_texel);
	vec4 tile = texture(u_match_terrain_cache,
			vec3(page_uv, u_match_terrain_page_layer));
	// ps_1_1: mad_d2 r0.rgb,t0.a,c1,c0; mul_x4 r0.rgb,r0,t0.
	// D3D's _d2 then _x4 modifiers reduce to a net x2.
	return tile.rgb *
			(obj_hemi_sky_color() + tile.a * obj_dir_light_color()) * 2.0;
	#else
	return vec3(0.0);
	#endif
}

// Return true only for the pixels that retail's earlier MATCHTERRAIN draw
// leaves visible after the following NORMAL draw. Opaque NORMAL always
// overwrites the pre-pass. For alpha-tested NORMAL, rejected pixels preserve
// the pre-pass terrain sample. This is algebraically the same two-pass result
// without duplicating/skinning the geometry through a second Godot material.
bool obj_match_terrain_replaces_normal(out vec3 terrain_color) {
	terrain_color = vec3(0.0);
	if (!obj_match_terrain_active()) {
		return false;
	}
	#ifdef OBJ_MATCHTERRAIN_SKINNED_STANCE
	vec4 base = obj_sample_base();
	if (obj_coverage_accepts(obj_surface_coverage(base))) {
		return false;
	}
	terrain_color = obj_match_terrain_color();
	return true;
	#else
	return false;
	#endif
}

// The distant MODEL foliage depth masks over a person entity. Retail draws a
// crouched/prone person's masks immediately inside its BySide wave, after
// the wave's MATCHTERRAIN pre-pass and before the wave's queued NORMAL
// draws, so every person of that wave (and of the camera-side wave after
// it, whose own pre-pass follows the far masks) depth-tests against them;
// FoliageMaskCompositorEffect rasterizes the same masks before Godot's
// opaque pass: R = the nearest far-wave mask depth, G = the nearest
// camera-wave one. u_foliage_mask_side is the person's wave (0 = not a
// person draw, 1 = far, 2 = camera side); the eye match keeps the mirror and
// every other camera unmasked. Retail Terrain_RenderSceneWithReflection
// @ 0x5c953e..0x5c9567 (far wave: MATCHTERRAIN subpass, then the wave with
// its masks), @ 0x5c9600..0x5c9647 (camera wave); Terrain_RenderSectorEntitiesBySide
// @ 0x5c7ddf..0x5c7f11 (masks) before render_sector_entity @ 0x5c7ffc.
global uniform sampler2D opennova_foliage_mask_depth;
global uniform bool opennova_foliage_mask_active;
global uniform vec3 opennova_foliage_mask_eye;
instance uniform float u_foliage_mask_side = 0.0;

// 0: no mask in front; 1: a mask of the fragment's own wave is in front, so
// only the pre-pass drawn before it remains; 2: a far-wave mask is in front
// of a camera-wave fragment, which hides its pre-pass too. Reverse-Z: a
// nearer mask stores the larger depth; the D3D LESSEQUAL test passes ties.
int obj_foliage_mask_outcome(vec4 frag_coord, vec3 camera_position_world) {
	if (u_foliage_mask_side < 0.5 || !opennova_foliage_mask_active ||
			distance(camera_position_world, opennova_foliage_mask_eye) >= 0.01) {
		return 0;
	}
	ivec2 size = textureSize(opennova_foliage_mask_depth, 0);
	ivec2 texel = clamp(ivec2(frag_coord.xy), ivec2(0), size - ivec2(1));
	vec2 masks = texelFetch(opennova_foliage_mask_depth, texel, 0).rg;
	if (u_foliage_mask_side < 1.5) {
		return masks.r > frag_coord.z ? 1 : 0;
	}
	if (masks.r > frag_coord.z) {
		return 2;
	}
	return masks.g > frag_coord.z ? 1 : 0;
}
