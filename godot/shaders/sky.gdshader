shader_type sky;

// The authored NovaLogic sky as the scene's real Sky background (ADR 0043 d3
// amendment): structural port of the original two-pass dome combine
// [orig: render_skybox @ 0x579080], evaluated per pixel by analytically
// intersecting the eye ray with the witnessed dome surface instead of
// rasterizing the 21x21 dome mesh:
//   pass 1 - textureless sky gradient, always sun-driven
//            [orig: SkyVS_GradientPassSource @ 0x7d7338]
//   pass 2 - both cloud layers in one multi-stage pass, driven by the ACTIVE
//            light (sun by day, moon at night), alpha-blended over pass 1
//            [orig: SkyVS_CloudPassSource @ 0x7d6fc0 + the 3-stage TSS table
//            decoded against RenderState_ApplyToDevice @ 0x681920]
// advanced_clouds=0 instead draws a flat cloud-color dome with the pass-1
// (NULL-texture) effect [orig: render_skybox @ 0x579b42..0x579bb6].
// The dome surface is the builder's sphere cap (env::build_sky_dome_mesh
// [orig: @ 0x578db0], the intersection oracle in engine/formats/env):
//   x^2 + z^2 + (y/s + C)^2 = R^2, y >= 0,  R = 3072, C = sqrt(2^23),
//   s = sky_height / (R - C), rim radius 1024 at y = 0,
// anchored at (eye.x, eye.y*0.5, eye.z) [orig: @ 0x5790d0] — so the eye sits
// at (0, eye.y*0.5, 0) in dome space and each pass reanchors for free (the
// water mirror renders this Sky with its own camera POSITION).
// Below the open rim the witnessed frame clear paints the miss region
// [orig: Render_ProcessMainSceneFrame @ 0x5ca776..0x5ca792]; the radiance
// (AT_CUBEMAP_PASS) lower hemisphere serves the .env hemi ground color, so
// REFLECTION_SOURCE_SKY reflects the authored sky above the horizon and the
// hemisphere ground below it.
// Tracked divergences (ledger register MP-8): the sun/light proximity term
// is a world-space dot (the original dp3'd normalized CLIP-space vectors
// [orig: c14 upload @ 0x57960e..0x579641]; sky shaders expose no projection),
// and the per-vertex 21x21 Gouraud interpolation becomes per-pixel.
// Full spec: docs/env/env-tod-re.md "Sky dome".

uniform vec3 u_sky_base : source_color = vec3(0.3, 0.4, 0.7); // c11 skybase
uniform vec3 u_sky_bright : source_color = vec3(0.5, 0.6, 0.9); // c11 + c12
uniform vec3 u_sky_highlight : source_color = vec3(1.0, 0.95, 0.8); // c15
uniform vec3 u_cloud_base : source_color = vec3(0.7, 0.75, 0.8); // c24
uniform vec3 u_cloud_highlight : source_color = vec3(0.6, 0.65, 0.7); // c27
uniform vec3 u_cloud_edge : source_color = vec3(0.3, 0.35, 0.4); // c26
uniform vec3 u_sun_dir = vec3(0.0, 1.0, 0.0); // pass-1 c13/c14 direction
uniform vec3 u_light_dir = vec3(0.0, 1.0, 0.0); // pass-2 c13/c14 direction
uniform vec3 u_fog_color : source_color = vec3(0.5, 0.7, 0.9); // sky-pass D3DRS_FOGCOLOR (skyfog)
uniform float u_fog_end = 1000.0; // world-unit fog end distance
uniform bool u_flat_pass = false; // advanced_clouds == 0
uniform vec3 u_flat_color : source_color = vec3(0.5, 0.5, 0.5); // cloud_rgb, flat path only

// The reference device applies anisotropic minification to active texture
// stages; this matters most at the dome's grazing-angle horizon. The cloud
// art decodes via source_color (linear scene, ADR 0043 amendment); the TSS
// alpha math is unaffected (the sRGB view leaves A linear).
uniform sampler2D u_cloud_tex1 : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_cloud_tex2 : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool u_has_clouds = false;
uniform vec2 u_scroll_offset1 = vec2(0.0);
uniform vec2 u_scroll_offset2 = vec2(0.0);
uniform float u_sky_height = 175.0;

// The witnessed 3-state frame clear (black indoors / doubled skyfog above
// water / Env_WaterColorLit underwater — environment_state.h carries the
// witness; world_device_frame.gd classifies the eye) fills the open
// below-rim region and the suppressed sky.
uniform vec3 u_frame_clear : source_color = vec3(0.0);
// Blink accum bit 0x2 (indoors) skips the visible skybox pass; the boot
// default is the idle black clear. The radiance pass deliberately ignores
// this — blink gates the visible pass, not lighting.
uniform bool u_sky_suppressed = true;
// The .env hemi ground color: the radiance map's lower hemisphere, so chrome
// (REFLECTION_SOURCE_SKY) reflects the authored ground tone below the
// horizon (the retired hemisphere_sky gradient's surviving half).
uniform vec3 u_hemi_ground : source_color = vec3(0.25, 0.23, 0.20);

// World-space proximity of the view ray to a direction — the reimpl mapping
// of the original clip-space dp3 (tracked, see the header).
float prox_to(vec3 eyedir, vec3 dir) {
	return max(dot(eyedir, normalize(dir)), 0.0);
}

vec3 dome_color(vec3 eyedir, float eye_world_y, out bool hit) {
	hit = false;
	// s = skyHeight / (3072 - sqrt(2^23)) [orig: v14 @ 0x578ed4]; epsilon
	// floor: a hand-authored sky_height 0 must not NaN-poison the dome.
	float dome_c = sqrt(8388608.0);
	float s = max(u_sky_height, 0.01) / (3072.0 - dome_c);
	// Eye in dome space: the anchor rides at half the camera height
	// [orig: @ 0x5790d0], so the eye sits at (0, eye.y * 0.5, 0).
	float o_y = eye_world_y * 0.5;
	// Intersect o + t*eyedir with the cap sphere (see the header equation).
	float a = o_y / s + dome_c;
	float b = eyedir.y / s;
	float a_q = 1.0 - eyedir.y * eyedir.y + b * b;
	float b_q = 2.0 * a * b;
	float k_q = a * a - 9437184.0; // R^2 = 3072^2 [orig: @ 0x7d75c8]
	float disc = b_q * b_q - 4.0 * a_q * k_q;
	if (disc < 0.0) {
		return vec3(0.0);
	}
	// The far root: from inside the cap the single positive hit; from above
	// it the far inner wall (what the mesh's cull_front rasterized).
	float t = (-b_q + sqrt(disc)) / (2.0 * a_q);
	if (t <= 0.0) {
		return vec3(0.0);
	}
	vec3 h = vec3(0.0, o_y, 0.0) + eyedir * t;
	if (h.y < -0.001) {
		// Below the open 1024-unit rim: the frame clear fills this region.
		return vec3(0.0);
	}
	hit = true;

	// Dome normal: the builder's anisotropic normal, NOT the vertex direction
	// — normalize(x, y_scaled / s^2, z) [orig: build_sky_dome_mesh @ 0x578fbb].
	vec3 dome_normal = normalize(vec3(h.x, h.y / (s * s), h.z));

	// Gradient driver t = 0.5*dot(N, dir) + 0.5; pass 1 always uses the sun,
	// pass 2 the active light [orig: SkyVS m3x3 v3,c4 / dp3 ..,c13 / mad].
	float t_sun = 0.5 * dot(dome_normal, u_sun_dir) + 0.5;
	float t_light = 0.5 * dot(dome_normal, u_light_dir) + 0.5;
	vec3 sky_sun = u_sky_base + (u_sky_bright - u_sky_base) * t_sun;
	vec3 sky_light = u_sky_base + (u_sky_bright - u_sky_base) * t_light;

	float prox_sun = prox_to(eyedir, u_sun_dir);
	float prox_light = prox_to(eyedir, u_light_dir);
	float p4_sun = prox_sun * prox_sun * prox_sun * prox_sun;
	float p8_sun = p4_sun * p4_sun;
	float p4_light = prox_light * prox_light * prox_light * prox_light;
	float p8_light = p4_light * p4_light;

	// vs_1_1 oD0/oD1 are color-register outputs: constants may exceed 1 after
	// the witnessed 2/255 upload, but the interpolated color inputs saturate.
	vec3 pass1 = clamp(mix(sky_sun, u_sky_highlight, p8_sun),
			vec3(0.0), vec3(1.0));
	vec3 sky_behind = clamp(mix(sky_light, u_cloud_edge, p4_light),
			vec3(0.0), vec3(1.0));
	vec3 cloud_ramp = clamp(mix(mix(u_cloud_base, u_cloud_edge, p4_light),
			u_cloud_highlight, p8_light), vec3(0.0), vec3(1.0));

	vec3 col;
	if (u_flat_pass) {
		// advanced_clouds == 0: flat cloud-color dome, no texture
		// [orig: render_skybox @ 0x579b42].
		col = u_flat_color;
	} else {
		col = pass1;
		if (u_has_clouds) {
			// The cloud UVs are the builder's PLANAR dome-space maps, layer 1
			// at 1/320 and layer 2 at 3/2048 [orig: kUv1Scale 0.003125
			// @ 0x7d75d0; kUv2Scale @ 0x7d75cc], recovered from the hit point.
			vec2 uv1 = h.xz * 0.003125 + u_scroll_offset1;
			vec2 uv2 = h.xz * 0.00146484375 + u_scroll_offset2;
			// 3-stage TSS cloud combine [orig: effect build @ 0x5789e0]:
			//   s1 color = tex0*tex1*2 (per-channel density, saturated)
			//   s2 color = LERP(density, cloudRamp, skyBehind)
			//   s1 alpha = tex0.a*tex1.a*2, s2 alpha = current^2*2 (saturated)
			// blended over pass 1 with srcAlpha/invSrcAlpha.
			vec4 c0 = texture(u_cloud_tex1, uv1);
			vec4 c1 = texture(u_cloud_tex2, uv2);
			vec3 density = min(c0.rgb * c1.rgb * 2.0, vec3(1.0));
			vec3 cloud_rgb = density * cloud_ramp + (1.0 - density) * sky_behind;
			float a1 = min(c0.a * c1.a * 2.0, 1.0);
			float a2 = min(a1 * a1 * 2.0, 1.0);
			col = mix(col, cloud_rgb, a2);
		}
	}

	// The sky wrapper selects the post-horizon-blend, doubled skyfog block by
	// the exact radial distance [orig: sky fog wrapper @ 0x579cb0; oFog =
	// 1 - dist/(0.9*fogDist), c9 upload @ 0x5792c2] — now per pixel.
	if (u_fog_end > 0.0) {
		float fog_w = 1.0 - t / max(0.9 * u_fog_end, 0.001);
		col = mix(u_fog_color, col, clamp(fog_w, 0.0, 1.0));
	}
	return clamp(col, vec3(0.0), vec3(1.0));
}

void sky() {
	bool hit = false;
	if (AT_CUBEMAP_PASS) {
		// The radiance map: the real sky above the horizon, the witnessed
		// hemisphere ground below — chrome (metallic environment techniques
		// on REFLECTION_SOURCE_SKY) reflects the authored sky.
		if (EYEDIR.y < 0.0) {
			COLOR = u_hemi_ground;
		} else {
			vec3 c = dome_color(EYEDIR, POSITION.y, hit);
			COLOR = hit ? c : u_fog_color;
		}
	} else if (u_sky_suppressed) {
		// Blink indoors / idle boot: the skybox pass is skipped; the frame
		// clear is all that draws [orig: the blink accum 0x2 gate].
		COLOR = u_frame_clear;
	} else {
		vec3 c = dome_color(EYEDIR, POSITION.y, hit);
		COLOR = hit ? c : u_frame_clear;
	}
}
