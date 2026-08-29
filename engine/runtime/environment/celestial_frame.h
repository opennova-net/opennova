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

#include <formats/env/env_celestial.h>

namespace opennova::env {

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

namespace detail {
inline int to_fixed_16_16(float value) {
	return static_cast<int>(value * 65536.0f);
}
} // namespace detail

// Overcast and SunDim are live end-to-end (env #27 — spring-smoothed in the
// weather core; target 0 in stock data).
inline CelestialBodyFrame build_sun_frame(const EnvironmentState &env,
		const Vec3 &cam_pos) {
	CelestialBodyFrame frame;
	frame.position = celestial_body_position(cam_pos, env.sun_direction());
	frame.tint = env.sun_color();
	frame.opacity = static_cast<float>(celestial_sun_alpha_fixed(
							detail::to_fixed_16_16(env.overcast_blend()),
							detail::to_fixed_16_16(env.sun_dim_pct()))) /
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
							detail::to_fixed_16_16(env.overcast_blend()),
							false)) /
			65536.0f;
	return frame;
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
							detail::to_fixed_16_16(1.0f), occlusion_brightness,
							detail::to_fixed_16_16(env.overcast_blend()),
							detail::to_fixed_16_16(env.sun_dim_pct()),
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
				   detail::to_fixed_16_16(1.0f), env.fog_level(),
				   detail::to_fixed_16_16(env.overcast_blend()),
				   detail::to_fixed_16_16(env.sun_dim_pct()), true)) /
			65536.0f;
}

} // namespace opennova::env
