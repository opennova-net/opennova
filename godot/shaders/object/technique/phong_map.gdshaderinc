// _psPhong2.fx ps11Phong2: Diffuse1.a controls both highlight brightness and
// the interpolation between the retail PhongMap's pow-4 and pow-64 lobes.
void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_lit, out float output_alpha) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	vec3 to_light = normalize(-opennova_light_block_dir);
	float ndotl = max(dot(normal, to_light), 0.0);
	float ndoth = max(dot(normal, normalize(to_light + view_dir)), 0.0);
	vec4 lookup = obj_phong_map_lookup(ndotl, ndoth);
	float specular = ndotl > 0.0 ? obj_phong_map_specular(lookup, base.a) : 0.0;
	vec3 ambient = (obj_hemi_ground_color() + obj_hemi_sky_color()) * 0.5;
	vec3 lit = (base.rgb * ambient +
			(base.rgb * lookup.a + vec3(specular)) * obj_dir_light_color() *
			v_dir_self_shadow) * 2.0;
	lit += obj_phong_map_points(base.rgb, base.a, normal, view_dir);
	lit = obj_add_local_light(lit, base.rgb, normal);
	output_lit = lit;
	// The normal pass is opaque; Diffuse1.a is material data, not coverage.
	output_alpha = 1.0;
}
