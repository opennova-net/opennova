// Glass.fx's GLOW technique samples the static CubeRotSpecular through the
// sun-aligned MatRotSpecular. Render_FillStaticCubemaps creates that cube from
// two coincident -Z lobes: white pow-800 at intensity 1.4 and warm pow-40 at
// intensity 1.0. Evaluating the generator analytically avoids an unwitnessed
// bitmap approximation while preserving the live sun rotation.
// [orig: Glass.fx TGlassFFP TECHNIQUE_GLOW;
// Render_FillStaticCubemaps @ 0x58f290;
// generate_cubemap_lighting @ 0x685bb0;
// apply_shader_parameters MatRotSpecular @ 0x58e14b].
vec3 obj_rotated_specular_cube(vec3 reflection_world) {
	// MatRotSpecular is built from Environment_GetLightDirectionFloat's
	// direct tuple (not the negated diffuse-light vector).
	vec3 to_sun = normalize(u_dir_light_dir);
	float aligned = max(dot(normalize(reflection_world), to_sun), 0.0);
	vec3 white_lobe = vec3(1.4) * pow(aligned, 800.0);
	vec3 warm_lobe = vec3(1.0, 248.0 / 255.0, 240.0 / 255.0) *
			pow(aligned, 40.0);
	return clamp(white_lobe + warm_lobe, vec3(0.0), vec3(1.0));
}

vec3 obj_glass_glow(vec3 camera_position_world) {
	vec3 view_dir = normalize(camera_position_world - v_world_pos);
	vec3 normal = normalize(v_geom_normal);
	vec3 reflection = reflect(-view_dir, normal);
	return obj_rotated_specular_cube(reflection) * u_reflect_color.rgb *
			u_color_src_global_gain * 2.0;
}

void obj_apply_q3_coverage(vec3 camera_position_world) {
	obj_apply_water_reflection_clip(camera_position_world);
	vec4 base = obj_sample_base();
	obj_apply_coverage(obj_surface_coverage(base));
}
