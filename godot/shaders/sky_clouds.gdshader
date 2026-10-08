shader_type spatial;
// Dome pass 2 (both cloud layers), alpha-blended srcAlpha/invSrcAlpha over
// pass 1 and the sun/moon discs, with z-write off and the culling of pass 1
// (sky.gdshaderinc carries the witnesses and the shared vertex stage).
render_mode unshaded, cull_back, depth_draw_never, blend_mix;

#include "res://shaders/sky.gdshaderinc"

#include "res://shaders/texture_filter.gdshaderinc"

// The sky maps are created with flags 0x100000, no 0x8, so the device samples
// them MIN/MAG LINEAR with MIPFILTER POINT whatever game.cfg's texfilter_level
// says [orig: Terrain_InitRenderingResources @ 0x578a82..0x578a9a;
// CGfxDevice_ApplyRenderStates @ 0x67e463..0x67e4a7] (engine
// renderer/texture_filter.h, TextureStage::SkyMap): bilinear on the nearest
// level of each map's chain. A PCX map is built from pixels, so its chain
// ends at 4 x 4, not at OpenNova's 1 x 1 [orig: GTexture_CreateFromPixelData_0
// @ 0x6877ba..0x6877e0]: never past u_cloud_max_lod1/2 (SkyDome's
// texture_max_lod; a .dds map keeps its own chain).
uniform sampler2D u_cloud_tex1 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_cloud_tex2 : filter_linear_mipmap, repeat_enable;
uniform float u_cloud_max_lod1 = 1000.0;
uniform float u_cloud_max_lod2 = 1000.0;


void fragment() {
	// 3-stage TSS cloud combine (retail effect build @ 0x5789e0):
	//   s1 color = tex0*tex1*2 (per-channel density, saturated)
	//   s2 color = LERP(density, cloudRamp, skyBehind)
	//   s1 alpha = tex0.a*tex1.a*2, s2 alpha = current^2*2 (each saturated)
	// The pass fogs its own colour toward the skyfog block before the blend
	// (the dome wrapper's device fog applies per pass).
	vec4 c0 = sample_point_mip(u_cloud_tex1, v_uv1, u_cloud_max_lod1);
	vec4 c1 = sample_point_mip(u_cloud_tex2, v_uv2, u_cloud_max_lod2);
	vec3 density = min(c0.rgb * c1.rgb * 2.0, vec3(1.0));
	vec3 cloud_rgb = density * v_cloud_ramp + (1.0 - density) * v_sky_behind;
	float a1 = min(c0.a * c1.a * 2.0, 1.0);
	ALBEDO = scene_output(sky_apply_fog(cloud_rgb));
	ALPHA = min(a1 * a1 * 2.0, 1.0);
}
