// Return true only for the pixels that retail's earlier MATCHTERRAIN draw
// leaves visible after the following NORMAL draw. Opaque NORMAL always
// overwrites the pre-pass. For alpha-tested NORMAL, rejected pixels preserve
// the pre-pass terrain sample. This is algebraically the same two-pass result
// without duplicating/skinning the geometry through a second Godot material.
bool obj_match_terrain_replaces_normal(out vec3 terrain_color) {
	terrain_color = vec3(0.0);
	#ifdef OBJ_MATCHTERRAIN_SKINNED_STANCE
	if (!u_match_terrain_enabled || !u_has_match_terrain_cache ||
			!u_match_terrain_page_ready) {
		return false;
	}
	vec4 base = obj_sample_base();
	if (obj_coverage_accepts(obj_surface_coverage(base))) {
		return false;
	}
	vec2 page_uv = (v_world_pos.xz - u_match_terrain_page_projection.xy) *
			u_match_terrain_page_projection.z;
	vec2 half_texel = vec2(0.5 / 256.0);
	page_uv = clamp(page_uv, half_texel, vec2(1.0) - half_texel);
	vec4 tile = texture(u_match_terrain_cache,
			vec3(page_uv, u_match_terrain_page_layer));
	// ps_1_1: mad_d2 r0.rgb,t0.a,c1,c0; mul_x4 r0.rgb,r0,t0.
	// D3D's _d2 then _x4 modifiers reduce to a net x2.
	terrain_color = tile.rgb *
			(obj_hemi_sky_color() + tile.a * obj_dir_light_color()) * 2.0;
	return true;
	#else
	return false;
	#endif
}
