// The per-frame celestial body state — the engine half of the shell's
// sun/moon/glare/star applier, ported from celestial.gd (2026-08-10
// de-scripting). The witnessed fixed-point math (body alphas, glare window +
// hysteresis, star generation/twinkle/cull, kCelestialBodyDistance) is
// env_celestial.h's (engine/formats/env); this header owns the per-frame
// selection the applier pushes into the celestial materials.
// Engine equivalents (docs/env/env-tod-re.md "Celestial bodies"):
// - [orig: EffectWorld_LoadCelestialModels @ 0x5adc50] resolves
//   sun_3di/moon_3di/star_3di/glare_3di (glare/star under additive mode
//   0x300000).
// - [orig: render_celestial_bodies @ 0x5acaa0] places sun/moon at
//   camera + direction * 64 (full camera height, identity rotation) with the
//   witnessed overcast/SunDim (sun) and fog-distance (moon @ 0x5acc40)
//   alphas; the world overdraws them, so depth-tested materials are the
//   structural equivalent.
// - [orig: render_skybox_sun_glow @ 0x5acd00] drives the glare: two jittered
//   terrain rays per frame into an 8-sample window + hysteresis brightness,
//   glow alpha = dot_view^4/2 x brightness x folds (env #14, closed).
// - The active light (sun by day, moon at night) drives the star near-light
//   cull [orig: Environment_GetLightDirectionFixed @ 0x57d8e0 at the field
//   loop].
#pragma once

#include <runtime/environment/environment_state.h>
#include <base/io/fixed.h>

#include <formats/env/env_celestial.h>

namespace opennova::env {

// One frame of the water-glint leg [orig: update_sun_glare @ 0x5ad130, once
// per main scene render from Terrain_RenderSceneWithReflection @ 0x5c96c0]:
// one sample per frame — the reflected-sun point on the water (with the
// 0.25 * (frame & 3) reflected-height jitter and the +-2 point x/z jitter),
// visible when the point sees BOTH the sun (point -> camera + sun * 2048) and
// the camera over terrain, then the +-16 chase toward popcount * 64 and the
// mirrored glare submit at camera + sun * 128 with the height term negated.
// Mission axes throughout (z = height); `segment_clear(a, b)` is the
// embedder's terrain line-of-sight between two mission points. The embedder
// checks for water first (no water = no glint) and owns the model writes.
struct WaterGlintFrame {
	bool visible = false;   // the accumulator saw the glint this frame
	Vec3 point_m;           // the sampled (jittered) water point
	Vec3 mirrored_sun;      // the sun with its height term negated
	float alpha = 0.0f;     // the submit alpha (0 hides the body)
};

template <typename SegmentClear>
inline WaterGlintFrame advance_water_glint(const EnvironmentState &state,
		const Vec3 &cam_m, const Vec3 &sun_m, const Vec3 &forward_m,
		WaterGlintState &glint, SegmentClear &&segment_clear) {
	WaterGlintFrame out;
	const float view_z_jitter = 0.25f * static_cast<float>(glint.frame_index & 3u);
	bool visible = water_glint_point(cam_m, sun_m, state.water_height(),
			view_z_jitter, out.point_m);
	if (visible) {
		// The +-2 unit point jitter [orig: @ 0x5ad26a..0x5ad27e] — mission x
		// and mission z (height).
		out.point_m.x += (glint.frame_index & 1u) ? 2.0f : -2.0f;
		out.point_m.z += (glint.frame_index & 2u) ? 2.0f : -2.0f;
		const Vec3 sun_far{cam_m.x + sun_m.x * 2048.0f, cam_m.y + sun_m.y * 2048.0f,
				cam_m.z + sun_m.z * 2048.0f};
		visible = segment_clear(out.point_m, sun_far) && segment_clear(out.point_m, cam_m);
	}
	water_glint_tick(glint, visible);
	out.visible = visible;
	// Placement: camera + sun * 128 with the HEIGHT term negated (the mirrored
	// glint below the eye [orig: @ 0x5ad1ba..0x5ad213 — the float matrix stores
	// (-(camY + sunY*128), camZ - sunZ*128, camX + sunX*128), the mission ->
	// render-float map of exactly that mirrored point]).
	out.mirrored_sun = Vec3{sun_m.x, sun_m.y, -sun_m.z};
	// Alpha: the view dot of the MIRRORED sun direction [orig: @ 0x5ad384
	// negates the height term before the view transform] through the witnessed
	// (dot^4 - 28672/65536) x brightness chain. Summed in the embedder's
	// (x, height, y) term order so the float rounding is the one it always had.
	const float view_dot = forward_m.x * out.mirrored_sun.x +
			forward_m.z * out.mirrored_sun.z + forward_m.y * out.mirrored_sun.y;
	const int dot_fixed = static_cast<int>(view_dot * 65536.0f);
	out.alpha = static_cast<float>(water_glint_alpha_fixed(dot_fixed, glint.brightness,
			io::float_to_fp16_16(state.sun_dim_pct()))) / 65536.0f;
	return out;
}

// One frame of a celestial body's placement + material inputs.
struct CelestialBodyFrame {
	Vec3 position{};
	Rgb tint;
	float opacity = 0.0f;
};

inline Vec3 celestial_body_position(const Vec3 &cam_pos, const Vec3 &dir) {
	return Vec3{cam_pos.x + dir.x * kCelestialBodyDistance,
			cam_pos.y + dir.y * kCelestialBodyDistance,
			cam_pos.z + dir.z * kCelestialBodyDistance};
}

// Overcast and SunDim are live end-to-end (env #27 — spring-smoothed in the
// weather core; target 0 in stock data).
inline CelestialBodyFrame build_sun_frame(const EnvironmentState &env,
		const Vec3 &cam_pos) {
	CelestialBodyFrame frame;
	frame.position = celestial_body_position(cam_pos, env.sun_direction());
	frame.tint = env.sun_color();
	frame.opacity = static_cast<float>(celestial_sun_alpha_fixed(
							io::float_to_fp16_16(env.overcast_blend()),
							io::float_to_fp16_16(env.sun_dim_pct()))) /
			65536.0f;
	return frame;
}

// The moon fades with the fog distance [orig: @ 0x5acc40].
inline CelestialBodyFrame build_moon_frame(const EnvironmentState &env,
		const Vec3 &cam_pos) {
	CelestialBodyFrame frame;
	frame.position = celestial_body_position(cam_pos, env.moon_direction());
	frame.tint = env.moon_color();
	frame.opacity = static_cast<float>(celestial_moon_alpha_fixed(
							env.fog_level(),
							io::float_to_fp16_16(env.overcast_blend()),
							false)) /
			65536.0f;
	return frame;
}

// The moon's opacity in the typed Q3 bloom-source draw. The bloom pass calls
// render_celestial_bodies(1), the fog-shader path: it sets
// CD3DDevice_SetFogAndBlendMode(&dword_3262260, 2) and the moon alpha branch
// takes the fog-shader leg, fogDistInt x 0.0002 x (1 - overcast), instead of
// the (fogDistInt - 400) / 600 ramp of the direct draw. The sun alpha has no
// fog-shader variant, so its Q3 opacity is the body opacity itself.
// [orig: render_celestial_bodies @ 0x5acaa0 (use_fog_shader @ 0x5acb80; moon
// alpha @ 0x5acc37..0x5acc61); FrameFX_RenderBloomPass @ 0x582a77].
inline float celestial_moon_q3_opacity(const EnvironmentState &env) {
	return static_cast<float>(celestial_moon_alpha_fixed(env.fog_level(),
				   io::float_to_fp16_16(env.overcast_blend()), true)) /
			65536.0f;
}

// The glare's PEAK opacity: the recovered fixed-point occlusion/brightness/
// dimming fold at dot=1. The shader applies the remaining positive dot^4
// factor from EACH pass camera, so the mirror view never inherits the main
// camera's glare angle (or its CPU visibility rejection).
inline CelestialBodyFrame build_glare_frame(const EnvironmentState &env,
		const Vec3 &cam_pos, int occlusion_brightness) {
	CelestialBodyFrame frame;
	frame.position = celestial_body_position(cam_pos, env.sun_direction());
	frame.tint = env.sun_color();
	// The locked reimpl profile is FBEFFECTS 3, so the direct draw carries
	// the witnessed quarter (the bloom re-adds the glare)
	// [orig: FrameFX_QualityAtLeast3 @ 0x581f60; >>= 2 @ 0x5ad033..0x5ad03c].
	frame.opacity = static_cast<float>(glare_glow_alpha_fixed(
							io::float_to_fp16_16(1.0f), occlusion_brightness,
							io::float_to_fp16_16(env.overcast_blend()),
							io::float_to_fp16_16(env.sun_dim_pct()),
							true)) /
			65536.0f;
	return frame;
}

// The glare's PEAK opacity in the typed Q3 bloom-source draw:
// FrameFX_RenderBloomPass draws the glow with NO occlusion test and the
// fog-based brightness [orig: render_skybox_sun_glow(0, 0) called from
// FrameFX_RenderBloomPass @ 0x582a77; the no-occlusion brightness
// @ 0x5ad013..0x5ad027]. The shader applies the per-pass dot^4 factor, like
// the main glare.
inline float glare_q3_peak_opacity(const EnvironmentState &env) {
	return static_cast<float>(glare_q3_alpha_fixed(
				   io::float_to_fp16_16(1.0f), env.fog_level(),
				   io::float_to_fp16_16(env.overcast_blend()),
				   io::float_to_fp16_16(env.sun_dim_pct()), true)) /
			65536.0f;
}

} // namespace opennova::env
