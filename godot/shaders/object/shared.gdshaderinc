// Geometry and authored material controls shared by the object techniques.
// Lighting, fog, and color management are owned by Godot's native Forward+
// environment; this layer preserves only the decoded object-material data.

uniform vec3 u_uv_transform_u = vec3(1.0, 0.0, 0.0);
uniform vec3 u_uv_transform_v = vec3(0.0, 1.0, 0.0);

varying vec2 v_uv;
varying vec2 v_uv2;
varying vec3 v_world_pos;
varying vec3 v_world_normal;
varying vec3 v_geom_normal;
varying vec3 v_world_tangent;
varying vec3 v_world_binormal;

// Full D3D COUNT2 affine transform. Coefficient rows preserve SET, shear,
// scroll, scale, and center rotation. [orig: compute_uv_transform_matrix @ 0x5B1990]
vec2 obj_transform_uv(vec2 uv) {
	vec3 uv1 = vec3(uv, 1.0);
	return vec2(dot(uv1, u_uv_transform_u), dot(uv1, u_uv_transform_v));
}
