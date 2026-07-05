#include "env/env_render.h"

#include <algorithm>
#include <cmath>

namespace opennova::env {

namespace {

constexpr float kLn64 = 4.1588830833596718565f; // ln(64)

int clamp_int(int value, int min_value, int max_value) {
	return std::max(min_value, std::min(max_value, value));
}

int rgb_byte(float normalized) {
	return clamp_int(static_cast<int>(normalized * 255.0f + 0.5f), 0, 255);
}

float byte_to_float(int byte_value) {
	return static_cast<float>(clamp_int(byte_value, 0, 255)) / 255.0f;
}

} // namespace

// ---------------------------------------------------------------------------
// Fog

FogParams compute_fog_params(int fog_type, float fog_end_distance, float overcast) {
	// [orig: Render_SetFogState @ 0x58a950] computes the per-type start with
	// density = overcast; [orig: CD3DDevice_SetFogParameters @ 0x677960] picks
	// linear vs exponential (useLinear = fog_type != 0) and disables on
	// start == end.
	FogParams params;
	params.end = fog_end_distance;
	const float inv_density = std::clamp(1.0f - overcast, 0.0f, 1.0f);
	switch (fog_type) {
	case 0:
		params.exponential = true;
		params.start = 0.0f;
		params.exp_density = fog_end_distance > 0.0f ? kLn64 / fog_end_distance : 0.0f;
		break;
	case 2:
		params.start = inv_density * fog_end_distance * 0.5f;
		break;
	case 3:
		params.start = inv_density * fog_end_distance * 0.25f;
		break;
	default: // type 1 and any other nonzero type
		params.start = 0.5f;
		break;
	}
	if (!params.exponential && params.start == params.end) {
		params.enabled = false;
	}
	return params;
}

float fog_end_above_water(float fog_distance, float overcast) {
	// end = dist * (0x10000 - overcast_fp/2) >> 16
	// [orig: Environment_GetFogEndDistance @ 0x57e426]
	const float clamped = std::clamp(overcast, 0.0f, 1.0f);
	return fog_distance * (1.0f - clamped * 0.5f);
}

float fog_end_underwater(float water_murk) {
	// [orig: Environment_GetFogEndDistance @ 0x57e40d]
	const float m = water_murk;
	const float vis = (1.0f - m) * ((1.0f - m) + 1.0f) * 0.5f;
	return (1.0f - (1.0f - vis) * 0.99199998f) * 200.0f;
}

// ---------------------------------------------------------------------------
// Day phase

DayPhase compute_day_phase(float hhmm_time) {
	// Windows in 16.16 hours [orig: Environment_ComputeTimeOfDayColors
	// @ 0x57de99..0x57df87]: 371376 = 05:40, 393216 = 06:00, 415056 = 06:20,
	// 1206948 = 18:25, 1228788 = 18:45, 1250628 = 19:05; ramp = 21840 (20 min).
	constexpr int kSunriseStart = 371376;
	constexpr int kSunriseSwitch = 393216;
	constexpr int kSunriseEnd = 415056;
	constexpr int kSunsetStart = 1206948;
	constexpr int kSunsetSwitch = 1228788;
	constexpr int kSunsetEnd = 1250628;
	constexpr float kRamp = 21840.0f;

	const int t = hhmm_to_hours_fp(hhmm_time);
	DayPhase phase;
	if (t >= kSunriseStart && t < kSunriseSwitch) {
		phase.is_night = true;
		phase.blend = static_cast<float>(kSunriseSwitch - t) / kRamp;
	} else if (t >= kSunriseSwitch && t < kSunriseEnd) {
		phase.is_night = false;
		phase.blend = static_cast<float>(t - kSunriseSwitch) / kRamp;
	} else if (t >= kSunriseEnd && t < kSunsetStart) {
		phase.is_night = false;
		phase.blend = 1.0f;
	} else if (t >= kSunsetStart && t < kSunsetSwitch) {
		phase.is_night = false;
		phase.blend = static_cast<float>(kSunsetSwitch - t) / kRamp;
	} else if (t >= kSunsetSwitch && t < kSunsetEnd) {
		phase.is_night = true;
		phase.blend = static_cast<float>(t - kSunsetSwitch) / kRamp;
	} else {
		phase.is_night = true;
		phase.blend = 1.0f;
	}
	return phase;
}

// ---------------------------------------------------------------------------
// Integer smoothing

int smooth_eighth(int current, int target) {
	// [orig: Environment_UpdateWeatherTick @ 0x57ee78] — eighth step with
	// overshoot snap.
	const int step = (target - current + 7) >> 3;
	const int next = current + step;
	if ((current < target && next > target) || (current > target && next < target)) {
		return target;
	}
	return next;
}

int spring_step(int current, int target, int step_clamp, int max_abs) {
	// [orig: Environment_UpdateWeatherTick @ 0x57ede2] — 1/32 step with step
	// and absolute clamps.
	int step = (target - current + 31) >> 5;
	step = clamp_int(step, -step_clamp, step_clamp);
	int next = current + step;
	next = clamp_int(next, -max_abs, max_abs);
	return next;
}

void ColorChannelState::snap_to(uint32_t packed) {
	b_fp = static_cast<int32_t>(packed & 0xFF) << 20;
	g_fp = static_cast<int32_t>((packed >> 8) & 0xFF) << 20;
	r_fp = static_cast<int32_t>((packed >> 16) & 0xFF) << 20;
	a_fp = static_cast<int32_t>((packed >> 24) & 0xFF) << 20;
}

uint32_t ColorChannelState::step(uint32_t target_packed, int max_step_fp) {
	// [orig: interpolate_weather_color @ 0x57d9e0] — per-channel (delta >> 3)
	// clamped, accumulate in 12.20, repack with +0x80000 rounding.
	const auto step_channel = [max_step_fp](int32_t &channel_fp, int target_byte) {
		int32_t delta = ((target_byte << 20) - channel_fp) >> 3;
		delta = clamp_int(delta, -max_step_fp, max_step_fp);
		channel_fp += delta;
	};
	step_channel(b_fp, static_cast<int>(target_packed & 0xFF));
	step_channel(g_fp, static_cast<int>((target_packed >> 8) & 0xFF));
	step_channel(r_fp, static_cast<int>((target_packed >> 16) & 0xFF));
	step_channel(a_fp, static_cast<int>((target_packed >> 24) & 0xFF));
	const auto repack = [](int32_t channel_fp) -> uint32_t {
		return static_cast<uint32_t>(clamp_int((channel_fp + 0x80000) >> 20, 0, 255));
	};
	return repack(b_fp) | (repack(g_fp) << 8) | (repack(r_fp) << 16) | (repack(a_fp) << 24);
}

// ---------------------------------------------------------------------------
// Lightning

int lightning_flash_level(const LightningStep *sequence, int count, int tick) {
	for (int i = 0; i < count; ++i) {
		if (sequence[i].tick == tick) {
			return sequence[i].level;
		}
	}
	return -1;
}

LightningAdditives lightning_additives(const Rgb &lightning_rgb, int level) {
	// [orig: Environment_SetLightningFlash @ 0x57d320] — lightning * level,
	// shifted per block: sky >> 8, fog/skyfog >> 9, ground >> 10.
	LightningAdditives out;
	const auto scaled = [&](float channel, int shift) {
		const int product = rgb_byte(channel) * clamp_int(level, 0, 255);
		return byte_to_float(product >> shift);
	};
	out.sky = {scaled(lightning_rgb.r, 8), scaled(lightning_rgb.g, 8), scaled(lightning_rgb.b, 8)};
	out.fog = {scaled(lightning_rgb.r, 9), scaled(lightning_rgb.g, 9), scaled(lightning_rgb.b, 9)};
	out.skyfog = out.fog;
	out.ground = {scaled(lightning_rgb.r, 10), scaled(lightning_rgb.g, 10), scaled(lightning_rgb.b, 10)};
	return out;
}

// ---------------------------------------------------------------------------
// Sun glare

GlareResult compute_sun_glare(float view_dot_sun, int occlusion_brightness) {
	// [orig: compute_sun_glare_and_fog_blend @ 0x5ad610]: dot^32 -> glare
	// (x192, clamp 255); dot^128 -> fog whitening (x40, clamp 40); both scaled
	// by the occlusion brightness (0..255).
	GlareResult result;
	if (view_dot_sun <= 0.0f) {
		return result;
	}
	const float dot = std::min(view_dot_sun, 1.0f);
	float pow32 = dot;
	for (int i = 0; i < 5; ++i) {
		pow32 *= pow32; // dot^32
	}
	const float pow128 = pow32 * pow32 * pow32 * pow32; // dot^128
	const float occlusion = static_cast<float>(clamp_int(occlusion_brightness, 0, 255)) / 255.0f;
	result.glare = clamp_int(static_cast<int>(pow32 * 192.0f * occlusion), 0, 255);
	result.fog_whiten = clamp_int(static_cast<int>(pow128 * 40.0f * occlusion), 0, 40);
	return result;
}

int glare_brightness_step(int current, int visible_rays) {
	// [orig: render_skybox_sun_glow @ 0x5acd00]: target = 32 * visible rays,
	// +-16 per frame, clamped 0..255.
	const int target = clamp_int(visible_rays, 0, 8) * 32;
	int next = current;
	if (current < target) {
		next = std::min(current + 16, target);
	} else if (current > target) {
		next = std::max(current - 16, target);
	}
	return clamp_int(next, 0, 255);
}

// ---------------------------------------------------------------------------
// Derived render colors

namespace {

Rgb weighted_add(const Rgb &light, const Rgb &base, int weight) {
	// (light * weight) >> 8 + base, saturating per byte.
	Rgb out;
	out.r = byte_to_float(((rgb_byte(light.r) * weight) >> 8) + rgb_byte(base.r));
	out.g = byte_to_float(((rgb_byte(light.g) * weight) >> 8) + rgb_byte(base.g));
	out.b = byte_to_float(((rgb_byte(light.b) * weight) >> 8) + rgb_byte(base.b));
	return out;
}

} // namespace

Rgb combine_terrain_light(const Rgb &light, const Rgb &sky) {
	// [orig: Environment_UpdateWeatherTick @ 0x57f0b3] — 0xB5/256 = 0.707.
	return weighted_add(light, sky, 0xB5);
}

Rgb combine_terrain_light_low(const Rgb &light, const Rgb &sky) {
	// [orig: Environment_UpdateWeatherTick @ 0x57f126] — 0x5A/256 = 0.352.
	return weighted_add(light, sky, 0x5A);
}

Rgb lit_water_color(const Rgb &water, const Rgb &combined_light) {
	// [orig: Environment_UpdateWeatherTick @ 0x57f16b] — (water * light) >> 7,
	// i.e. water * light * 2 in normalized space, saturating.
	Rgb out;
	out.r = byte_to_float((rgb_byte(water.r) * rgb_byte(combined_light.r)) >> 7);
	out.g = byte_to_float((rgb_byte(water.g) * rgb_byte(combined_light.g)) >> 7);
	out.b = byte_to_float((rgb_byte(water.b) * rgb_byte(combined_light.b)) >> 7);
	return out;
}

Rgb double_saturate(const Rgb &color) {
	// [orig: Environment_UpdateWeatherTick @ 0x57f17c/0x57f190] — paddusb(c, c).
	Rgb out;
	out.r = byte_to_float(rgb_byte(color.r) * 2);
	out.g = byte_to_float(rgb_byte(color.g) * 2);
	out.b = byte_to_float(rgb_byte(color.b) * 2);
	return out;
}

Rgb horizon_blend_skyfog(const Rgb &fog, const Rgb &skyfog,
                         uint32_t fog_dist_fixed, uint32_t fog_dist_reference_fixed) {
	// [orig: Environment_UpdateWeatherTick @ 0x57e9b0 — half shr @ 0x57f03d,
	//  quarter shr @ 0x57f04c, unsigned strict compare @ 0x57f04e, borrow clamp
	//  @ 0x57f058, t = div/shr16 @ 0x57f061..0x57f063, per-byte MMX
	//  @ 0x57f066..0x57f0a1 written IN PLACE over skyfog[0].]
	const uint32_t half = fog_dist_reference_fixed >> 1;
	const uint32_t quarter = half >> 1;
	if (fog_dist_fixed >= half || half == quarter) return skyfog;
	const uint32_t num = (fog_dist_fixed > quarter) ? fog_dist_fixed - quarter : 0;
	const uint32_t t = static_cast<uint32_t>(
		((static_cast<uint64_t>(num) << 32) / (half - quarter)) >> 16); // 0.16 fraction
	const int tw = static_cast<int>(t >> 1);              // pmulhw operand (skyfog side)
	const int tiw = static_cast<int>((t ^ 0xFFFFu) >> 1); // pmulhw operand (fog side)
	const auto channel = [&](float fog_c, float sky_c) {
		const int fogw = (rgb_byte(fog_c) * 0x101) >> 1; // punpcklbw x,x ; psrlw 1
		const int skyw = (rgb_byte(sky_c) * 0x101) >> 1;
		int res = ((fogw * tiw) >> 16) + ((skyw * tw) >> 16); // pmulhw pair
		if (res > 0x7FFF) res = 0x7FFF;                       // paddsw saturation
		return byte_to_float(res >> 6);                       // psrlw 6 ; packuswb
	};
	Rgb out;
	out.r = channel(fog.r, skyfog.r);
	out.g = channel(fog.g, skyfog.g);
	out.b = channel(fog.b, skyfog.b);
	return out;
}

// ---------------------------------------------------------------------------
// BMS overrides

void apply_bms_overrides(Config &config, const BmsEnvOverrides &overrides) {
	// [orig: Game_LoadTerrainDuringConnect @ 0x520710 + Game_StartMission
	// @ 0x525371..0x525399]
	if (overrides.has_water_height) {
		config.water_height = overrides.water_height;
		config.water_height_set = true;
	}
	if (overrides.has_fog_level) {
		config.fog_level = overrides.fog_level;
	}
	if (overrides.has_fog_color) {
		// The engine writes the packed color into the fog block's parsed
		// target, overriding every keyframe's fog contribution from then on;
		// we override the keyframe fog colors at the config level.
		for (Keyframe &keyframe : config.keyframes) {
			keyframe.fog = overrides.fog_color;
		}
	}
	if (overrides.has_water_color) {
		config.water_rgb = overrides.water_color;
	}
	if (overrides.has_water_murk) {
		config.water_murk = std::min(overrides.water_murk, 0.99f);
	}
	if (overrides.has_start_time) {
		config.curtime = overrides.start_time;
	}
}

TerrainTint terrain_tint_from_packed(uint32_t terrain_color_packed) {
	// [orig: PolyTrn_SetTerrainTintColors @ 0x605e20] full @ 0x31a1824,
	// half @ 0x31a1828.
	TerrainTint tint;
	tint.full = terrain_color_packed | 0xFF000000u;
	tint.half = ((terrain_color_packed >> 1) & 0x007F7F7Fu) | 0xFF000000u;
	return tint;
}

TerrainTint terrain_tint_from_rgb(const Rgb &terrain_rgb) {
	const uint32_t packed = (static_cast<uint32_t>(rgb_byte(terrain_rgb.r)) << 16) |
			(static_cast<uint32_t>(rgb_byte(terrain_rgb.g)) << 8) |
			static_cast<uint32_t>(rgb_byte(terrain_rgb.b));
	return terrain_tint_from_packed(packed);
}

uint32_t foliage_lightmap_tint(uint32_t texel_argb, uint32_t full_tint) {
	// [orig: sample_terrain_lightmap @ 0x606030] per channel
	// min((texel * FULL) >> 7, 255); alpha passthrough.
	uint32_t out = texel_argb & 0xFF000000u;
	for (int shift = 0; shift <= 16; shift += 8) {
		const uint32_t texel_c = (texel_argb >> shift) & 0xFFu;
		const uint32_t tint_c = (full_tint >> shift) & 0xFFu;
		const uint32_t tinted = std::min<uint32_t>((texel_c * tint_c) >> 7, 255u);
		out |= tinted << shift;
	}
	return out;
}

Rgb tile_overlay_tint_factor(const TerrainTint &tint) {
	// TEXTURE x DIFFUSE(HALF) under MODULATE2X -> single-multiply factor
	// 2*HALF/255 per channel [orig: PolyTrn_RenderTile @ 0x60df0d].
	Rgb factor;
	factor.r = static_cast<float>(2u * ((tint.half >> 16) & 0xFFu)) / 255.0f;
	factor.g = static_cast<float>(2u * ((tint.half >> 8) & 0xFFu)) / 255.0f;
	factor.b = static_cast<float>(2u * (tint.half & 0xFFu)) / 255.0f;
	return factor;
}

} // namespace opennova::env
