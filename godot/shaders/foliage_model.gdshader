shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque;

// Retail MODEL foliage pass.
//
// Foliage_GridPlacementVS performs the bound-square biquadratic ground fit
// and height-weighted sway. The pass object binds :fd at T0, selects diffuse
// for RGB, and the VS supplies c6=(0,0,0,1) [orig: Foliage_DrawModelTileSlot
// @ 0x601df1 passes (0,0,0,1)]. The model draw runs with D3DRS_FOGENABLE
// OFF: the pass word 0x00440000 carries no FOGENABLE (0x20000) bit and
// CGfxShader_ApplyPass writes the fog state unconditionally, so the c5 fog
// constants are dead on this pass - retail far models are UNFOGGED black
// alpha silhouettes [orig: apply mode 0 @ 0x601e33; fog latch
// @ CGfxShader_ApplyPass 0x68324f]. This intentionally does not share the
// FAR foliage lighting/fog contract.

uniform sampler2D u_fd_texture : filter_linear_mipmap, repeat_enable;
// Anchor-derived clamp(int(4096 / (distance_units + 1)), 8, 128).
uniform float u_model_alpha_ref : hint_range(8.0, 128.0, 1.0) = 8.0;

// Bound square in pre-scale Godot mesh space. The host bakes the witnessed
// 3DI-to-Godot axis mapping into the instance payload.
uniform vec2 u_bound_center = vec2(0.0);
uniform float u_bound_radius = 1.0;
// Per-tile counter * 0.001 [Foliage_ModelWindPhaseCounter @ 0x3162170].
uniform float u_model_wind_phase = 0.0;


void vertex() {
	float inv_span = 1.0 / (2.0 * max(u_bound_radius, 1e-6));
	float x_n = (VERTEX.x - u_bound_center.x) * inv_span + 0.5;
	float z_n = (VERTEX.z - u_bound_center.y) * inv_span + 0.5;

	// Bilinear corners plus the witnessed biquadratic midpoint fold.
	// COLOR=(corner height - hbase), INSTANCE_CUSTOM=(E_B,T_B,E_A,T_A)
	// after the importer-axis mapping.
	vec4 weights = vec4(
		(1.0 - x_n) * (1.0 - z_n),
		x_n * (1.0 - z_n),
		(1.0 - x_n) * z_n,
		x_n * z_n
	);
	float ground_delta = dot(weights, COLOR);
	float u = 2.0 * x_n - 1.0;
	float v = 2.0 * z_n - 1.0;
	ground_delta += (1.0 - v * v) * (INSTANCE_CUSTOM.x + u * INSTANCE_CUSTOM.y)
		+ (1.0 - u * u) * (INSTANCE_CUSTOM.z + v * INSTANCE_CUSTOM.w);

	vec3 world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	world_pos.y += ground_delta;

	// Retail render Z is negative Godot world Z.
	float y_half = VERTEX.y * 0.5;
	world_pos.z -= y_half * sin(u_model_wind_phase) * 0.08;

	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(world_pos, 1.0);
}


void fragment() {
	vec4 fd = texture(u_fd_texture, UV);
	float alpha_threshold = clamp(u_model_alpha_ref, 8.0, 128.0) / 255.0;
	if (fd.a < alpha_threshold) {
		discard;
	}

	// Stage0 RGB selects diffuse, GridPlacementVS emits black c6, and the
	// pass draws with fog DISABLED (see the header witness): the silhouette
	// stays pure black at every distance, exactly as retail renders it.
	vec3 result = vec3(0.0);

	ALBEDO = result;
	ALPHA = fd.a;
	ALPHA_SCISSOR_THRESHOLD = alpha_threshold;
}
