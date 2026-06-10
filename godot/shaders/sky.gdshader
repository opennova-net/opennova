shader_type spatial;
render_mode unshaded, cull_front;

// Sky-dome shader fed by the ENV/TOD path:
// [orig: Environment_ComputeTimeOfDayColors @ 0x57de40] interpolates colors and
// [orig: render_skybox @ 0x579080] consumes them; the sky pass consumes skybase,
// skybright, skyhighlight, cloudbase/highlight/edge, skyfog, sun, and moon.

uniform vec3 u_sky_base = vec3(0.3, 0.4, 0.7);
uniform vec3 u_sky_bright = vec3(0.5, 0.6, 0.9);
uniform vec3 u_sky_highlight = vec3(1.0, 0.95, 0.8);
uniform vec3 u_horizon_color = vec3(0.7, 0.75, 0.8);
uniform vec3 u_ground_fog_color = vec3(0.6, 0.65, 0.7);
uniform vec3 u_secondary_ambient = vec3(0.3, 0.35, 0.4);
uniform vec3 u_cloud_tint = vec3(0.5, 0.5, 0.5);
uniform vec3 u_sun_dir = vec3(0.0, 1.0, 0.0);
uniform vec3 u_moon_dir = vec3(0.0, -1.0, 0.0);
uniform vec3 u_sun_color = vec3(1.0, 0.9, 0.7);
uniform vec3 u_moon_color = vec3(0.3, 0.35, 0.5);
uniform vec3 u_clear_color = vec3(0.5, 0.6, 0.8);

uniform sampler2D u_cloud_tex1 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_cloud_tex2 : filter_linear_mipmap, repeat_enable;
uniform bool u_has_clouds = false;
uniform vec2 u_scroll_offset1 = vec2(0.0);
uniform vec2 u_scroll_offset2 = vec2(0.0);
uniform float u_sky_height = 175.0;

varying vec3 v_world_pos;
varying float v_elevation;
varying vec2 v_uv1;
varying vec2 v_uv2;

void vertex() {
	float height_scale = u_sky_height / 175.69;
	vec3 scaled = VERTEX * height_scale;
	v_world_pos = (MODEL_MATRIX * vec4(scaled, 1.0)).xyz;
	v_elevation = clamp(scaled.y / u_sky_height, 0.0, 1.0);
	v_uv1 = UV;
	v_uv2 = UV2;
	VERTEX = scaled;
}

void fragment() {
	vec3 view_dir = normalize(v_world_pos - CAMERA_POSITION_WORLD);
	float sun_facing = max(dot(view_dir, u_sun_dir), 0.0);
	sun_facing = sun_facing * 0.5 + 0.5;
	vec3 zenith_color = u_sky_base + (u_sky_bright - u_sky_base) * sun_facing;
	float fog_blend = 1.0 - smoothstep(0.0, 0.1, v_elevation);
	vec3 horizon_color = mix(u_horizon_color, u_ground_fog_color, fog_blend);
	zenith_color += u_secondary_ambient * 0.5;
	float elev_blend = smoothstep(0.0, 0.4, v_elevation);
	vec3 sky = mix(horizon_color, zenith_color, elev_blend);

	if (u_has_clouds) {
		float cloud1 = texture(u_cloud_tex1, v_uv1 + u_scroll_offset1).r;
		float cloud2 = texture(u_cloud_tex2, v_uv2 + u_scroll_offset2).r;
		float cloud_density = cloud1 * 0.7 + cloud2 * 0.3;
		vec3 cloud_color = mix(u_horizon_color, u_ground_fog_color, cloud_density);
		cloud_color *= u_cloud_tint * 2.0;
		float cloud_fade = smoothstep(0.05, 0.3, v_elevation);
		sky = mix(sky, cloud_color, cloud_density * cloud_fade * 0.8);
	}

	float sun_dot = max(dot(view_dir, u_sun_dir), 0.0);
	sky += u_sky_highlight * pow(sun_dot, 64.0) * 2.0;
	sky += u_sun_color * pow(sun_dot, 4.0) * 0.15;
	float moon_dot = max(dot(view_dir, u_moon_dir), 0.0);
	sky += u_moon_color * pow(moon_dot, 48.0);
	float clear_blend = 1.0 - smoothstep(0.0, 0.05, v_elevation);
	sky = mix(sky, u_clear_color, clear_blend);
	ALBEDO = sky;
}
