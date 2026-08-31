// BmTxMirrT.fx TSegTanTexMirror P0/P1: textured mirror; the P3
// postmultiply layer remains its own draw (postmultiply/).
ObjSurface obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	ObjSurface s = obj_surface_defaults(base, normal);
	s.metallic = 0.6;
	s.roughness = 0.15;
	s.specular = 0.5;
	return s;
}
