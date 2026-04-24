shader_type spatial;
render_mode unshaded, blend_mix, cull_disabled;

// Water pass consuming .env water_rgb/water_murk plus TOD fog output.
// Engine equivalents: sub_53E3F0@0x53E3F0 parses the static water fields,
// sub_53FCC0@0x53FCC0 supplies per-frame fog color.

uniform vec3 u_water_color = vec3(0.408, 0.314, 0.224);
uniform float u_water_alpha = 0.6;
uniform float u_time = 0.0;
uniform float u_scroll_speed = 0.00343;
uniform vec3 u_fog_color = vec3(0.5, 0.6, 0.8);
uniform float u_fog_start = 300.0;
uniform float u_fog_end = 1000.0;
uniform int u_fog_type = 2;

varying vec3 v_world_pos;

void vertex() {
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	vec2 uv = v_world_pos.xz * 0.015625;
	uv += vec2(u_time * u_scroll_speed, u_time * u_scroll_speed);
	float wave1 = sin(uv.x * 6.28 + u_time * 0.25) * 0.015;
	float wave2 = cos(uv.y * 6.28 + u_time * 0.18) * 0.015;
	uv += vec2(wave1, wave2);

	float wave_bright = 0.95 + 0.05 * sin(uv.x * 12.56 + uv.y * 9.42 + u_time * 0.3);
	vec3 color = u_water_color * wave_bright;
	vec3 view_dir = normalize(CAMERA_POSITION_WORLD - v_world_pos);
	float cos_angle = max(dot(view_dir, vec3(0.0, 1.0, 0.0)), 0.0);
	float fresnel = pow(1.0 - cos_angle, 2.0);
	float alpha = mix(u_water_alpha, 0.95, fresnel);

	float dist = length(v_world_pos - CAMERA_POSITION_WORLD);
	float fog_range = max(u_fog_end - u_fog_start, 1.0);
	float density = 1.0 / fog_range;
	float fog_factor;
	if (u_fog_type == 1) {
		fog_factor = exp(-dist * density);
	} else if (u_fog_type == 2) {
		fog_factor = exp(-(dist * density) * (dist * density));
	} else if (u_fog_type == 3) {
		fog_factor = exp(-dist * density * 0.5);
	} else {
		fog_factor = (u_fog_end - dist) / fog_range;
	}
	fog_factor = clamp(fog_factor, 0.0, 1.0);
	color = mix(u_fog_color, color, fog_factor);

	ALBEDO = color;
	ALPHA = alpha;
}
