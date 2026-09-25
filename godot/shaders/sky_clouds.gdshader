shader_type spatial;
// Dome pass 2 (both cloud layers), alpha-blended srcAlpha/invSrcAlpha over
// pass 1 and the sun/moon discs, with z-write off like pass 1
// (sky.gdshaderinc carries the witnesses and the shared vertex stage).
render_mode unshaded, cull_front, depth_draw_never, blend_mix;

#include "res://shaders/sky.gdshaderinc"

// The reference device applies anisotropic minification to active texture
// stages; this matters most at the dome's grazing-angle horizon.
uniform sampler2D u_cloud_tex1 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_cloud_tex2 : filter_linear_mipmap_anisotropic, repeat_enable;


void fragment() {
	// 3-stage TSS cloud combine (retail effect build @ 0x5789e0):
	//   s1 color = tex0*tex1*2 (per-channel density, saturated)
	//   s2 color = LERP(density, cloudRamp, skyBehind)
	//   s1 alpha = tex0.a*tex1.a*2, s2 alpha = current^2*2 (each saturated)
	// The pass fogs its own colour toward the skyfog block before the blend
	// (the dome wrapper's device fog applies per pass).
	vec4 c0 = texture(u_cloud_tex1, v_uv1);
	vec4 c1 = texture(u_cloud_tex2, v_uv2);
	vec3 density = min(c0.rgb * c1.rgb * 2.0, vec3(1.0));
	vec3 cloud_rgb = density * v_cloud_ramp + (1.0 - density) * v_sky_behind;
	float a1 = min(c0.a * c1.a * 2.0, 1.0);
	ALBEDO = scene_output(sky_apply_fog(cloud_rgb));
	ALPHA = min(a1 * a1 * 2.0, 1.0);
}
