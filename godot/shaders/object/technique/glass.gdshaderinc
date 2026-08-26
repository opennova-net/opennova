// Glass.fx TGlassFFP NORMAL: TexCubeEnvironment * ReflectColor *
// ColorSrcGlobalGain under MODULATE2X, additive and ZMODE_NOWRITE.
void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_lit, out float output_alpha) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	vec3 environment = obj_environment_cube_approx(reflect(-view_dir, normal));
	output_lit = environment * u_reflect_color.rgb * opennova_light_block_gain * 2.0;
	output_alpha = base.a;
}
