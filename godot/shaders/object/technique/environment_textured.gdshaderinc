// BmTxMirrT.fx TSegTanTexMirror P0/P1. P0 replaces the framebuffer with the
// cube reflection under FOGMODE_SHADERSET (the set fog colour is 0x7f7f7f),
// then P1 adds each point-specular lobe under black FOGMODE_SHADERADD. P3 is
// deliberately a separate geometry draw in postmultiply/environment_textured:
// its raw-UV Diffuse1 and DESTCOLOR/SRCCOLOR blend cannot be folded into P0
// without changing independent fog, alpha-test, and framebuffer semantics.
void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_lit, out float output_alpha) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	vec3 reflection = reflect(-view_dir, normal);
	vec3 reflected = obj_environment_cube_approx(reflection) * u_reflect_color.rgb;
	vec3 point_specular = obj_mirror_points(normal, view_dir);
	if (opennova_fog_enabled) {
		float visibility = obj_scene_fog_visibility(camera_position_world);
		reflected = mix(vec3(127.0 / 255.0), reflected, visibility);
		point_specular *= visibility;
	}
	output_lit = reflected + point_specular;
	output_alpha = base.a;
}
