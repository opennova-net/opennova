#include "env/env_celestial.h"
#include "env/env_water_render.h"
#include "env/env_weather.h"

#include <algorithm>
#include <cmath>
#include <iterator>

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

void EnvScalarChannels::tick() {
	// The witnessed in-tick order [orig: Environment_UpdateWeatherTick scalar
	// tail @ 0x57edd7..0x57ef92]; the FOV eighth-snap (@ 0x57ee62) and the
	// cloud-scroll eighth-snap (@ 0x57eecc, CloudScrollState) interleave here
	// in the original and live with their owners.
	fog_dist_fp = spring_step(fog_dist_fp, fog_dist_target_fp, fog_step_fp, fog_max_fp);
	sun_dim_fp = spring_step(sun_dim_fp, sun_dim_target_fp, sun_dim_step_fp, sun_dim_max_fp);
	sky_height_fp = smooth_eighth(sky_height_fp, sky_height_target_fp);
	rain_pct_fp = spring_step(rain_pct_fp, rain_pct_target_fp, rain_step_fp, rain_max_fp);
	overcast_fp = spring_step(overcast_fp, overcast_target_fp, overcast_step_fp, overcast_max_fp);
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

LightningAdditivesPacked lightning_additives_packed(uint32_t lightning_packed, int level) {
	// [orig: Environment_SetLightningFlash @ 0x57d320] — pmullw(bytes, level)
	// then psrlw per slot; exact byte truncation (no rounding).
	LightningAdditivesPacked out;
	const int lvl = clamp_int(level, 0, 255);
	const auto slot = [&](int shift) -> uint32_t {
		uint32_t packed = 0;
		for (int byte_shift = 0; byte_shift < 32; byte_shift += 8) {
			const uint32_t channel = (lightning_packed >> byte_shift) & 0xFF;
			packed |= ((channel * static_cast<uint32_t>(lvl)) >> shift & 0xFF) << byte_shift;
		}
		return packed;
	};
	out.sky = slot(8);
	out.fog = slot(9);
	out.skyfog = out.fog;
	out.ground = slot(10);
	return out;
}

bool LightningSequencers::tick() {
	// [orig: Environment_UpdateWeatherTick @ 0x57ec6f / @ 0x57ed0a] — each
	// active timer decrements, then the remaining value is matched against
	// the epoch table; a hit SETS the flash level (Environment_SetLightningFlash
	// overwrites, never maxes). A processes before B, so a same-tick collision
	// resolves to B's level, as in the original's statement order.
	bool set = false;
	if (timer_a) {
		--timer_a;
		const int lvl = lightning_flash_level(
				kLightningSequenceA, static_cast<int>(std::size(kLightningSequenceA)), timer_a);
		if (lvl >= 0) {
			level = lvl;
			set = true;
		}
	}
	if (timer_b) {
		--timer_b;
		const int lvl = lightning_flash_level(
				kLightningSequenceB, static_cast<int>(std::size(kLightningSequenceB)), timer_b);
		if (lvl >= 0) {
			level = lvl;
			set = true;
		}
	}
	return set;
}

// ---------------------------------------------------------------------------
// Weather oscillator

uint32_t WeatherOscillator::reroll() {
	// [orig: Environment_UpdateWeatherTick @ 0x57e9fc..0x57ea16] — rol 9, then
	// the SIGNED carry: ((int32)rotated >> 31) & 0x1ABB09 (x86 cdq/and/add).
	const uint32_t rotated = (prng << 9) | (prng >> 23);
	prng = rotated + (static_cast<uint32_t>(static_cast<int32_t>(rotated) >> 31) & 0x1ABB09u);
	return prng;
}

int WeatherOscillator::tick() {
	// [orig: Environment_UpdateWeatherTick @ 0x57ea16..0x57eaed] — square-
	// weighted noise, 256-entry rings, then the 1/32 + 63/64 spring pair.
	const int noise = static_cast<int>(reroll() & 0xFFFu);
	const int scaled = (intensity * (15 * prev_noise + ((noise * noise) >> 8))) >> 12;
	ring_index = static_cast<uint8_t>(ring_index + 1);
	amp_ring[ring_index] = std::max(0, 0xFFFF - 2 * scaled);
	prev_noise = scaled;
	pos += velocity + scaled;
	smoothed = (pos + 31 * smoothed) >> 5;
	velocity = (63 * (velocity + ((0x8000 - pos) >> 4))) >> 6;
	smoothed = clamp_int(smoothed, 0, 0xFFFF);
	osc_ring[ring_index] = smoothed;
	return scaled;
}

// ---------------------------------------------------------------------------
// Rain + weather color blocks

int rain_blend_factor(int rain_intensity) {
	// [orig: interpolate_weather_color @ 0x57d9e0] — the unsigned over-range
	// check zeroes the factor, otherwise 0x8000 - intensity.
	if (static_cast<uint32_t>(rain_intensity) > 0x8000u) {
		return 0;
	}
	return 0x8000 - rain_intensity;
}

namespace {

uint32_t paddusb(uint32_t a, uint32_t b) {
	uint32_t out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		const uint32_t sum = ((a >> shift) & 0xFF) + ((b >> shift) & 0xFF);
		out |= (sum > 0xFF ? 0xFFu : sum) << shift;
	}
	return out;
}

} // namespace

void WeatherColorBlock::snap(uint32_t packed) {
	channels.snap_to(packed);
	render_color = packed;
	pre_mod_color = packed;
	target = packed;
}

void WeatherColorBlock::set_step_deltas(int frames) {
	// [orig: ColorBlock_SetStepDeltas @ 0x57d940] — per channel:
	// |target_byte << 20 + frames/2 - current| / frames (the +frames/2 rounds
	// the numerator before the truncating divide).
	if (frames == 0) {
		frames = 1;
	}
	const auto rate_for = [frames](int target_byte, int32_t channel_fp) -> int32_t {
		const int32_t delta = (target_byte << 20) + (frames >> 1) - channel_fp;
		const int32_t magnitude = delta < 0 ? -delta : delta;
		return magnitude / frames;
	};
	max_rate[0] = rate_for(static_cast<int>(target & 0xFF), channels.b_fp);
	max_rate[1] = rate_for(static_cast<int>((target >> 8) & 0xFF), channels.g_fp);
	max_rate[2] = rate_for(static_cast<int>((target >> 16) & 0xFF), channels.r_fp);
	max_rate[3] = rate_for(static_cast<int>((target >> 24) & 0xFF), channels.a_fp);
}

void WeatherColorBlock::tick(uint32_t modulator_packed, int rain_intensity) {
	// [orig: interpolate_weather_color @ 0x57d9e0] — the full block pipeline.
	// Step: per-channel (delta >> 3) clamped to that channel's max rate,
	// accumulate in 12.20, repack with +0x80000 rounding.
	const auto step_channel = [](int32_t &channel_fp, int target_byte, int32_t rate) {
		int32_t delta = ((target_byte << 20) - channel_fp) >> 3;
		delta = clamp_int(delta, -rate, rate);
		channel_fp += delta;
	};
	step_channel(channels.b_fp, static_cast<int>(target & 0xFF), max_rate[0]);
	step_channel(channels.g_fp, static_cast<int>((target >> 8) & 0xFF), max_rate[1]);
	step_channel(channels.r_fp, static_cast<int>((target >> 16) & 0xFF), max_rate[2]);
	step_channel(channels.a_fp, static_cast<int>((target >> 24) & 0xFF), max_rate[3]);
	const auto repack = [](int32_t channel_fp) -> uint32_t {
		return static_cast<uint32_t>(clamp_int((channel_fp + 0x80000) >> 20, 0, 255));
	};
	const uint32_t stepped = repack(channels.b_fp) | (repack(channels.g_fp) << 8) |
			(repack(channels.r_fp) << 16) | (repack(channels.a_fp) << 24);

	// Additive: the lightning slot saturate-adds onto the stepped color
	// (paddusb into state[1]).
	pre_mod_color = paddusb(stepped, additive);

	// Modulator x rain: out_c = ((c * m) >> 1) * (factor >> 4) >> 16, packed
	// with unsigned saturation (pmullw / psrlw 1 / pmulhw / packuswb). The
	// identity modulator byte is 64 (with rain 0 the chain is exact identity).
	const uint32_t factor = static_cast<uint32_t>(rain_blend_factor(rain_intensity)) >> 4;
	uint32_t modulated = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		const uint32_t c = (pre_mod_color >> shift) & 0xFF;
		const uint32_t m = (modulator_packed >> shift) & 0xFF;
		const uint32_t value = (((c * m) >> 1) * factor) >> 16;
		modulated |= (value > 0xFF ? 0xFFu : value) << shift;
	}
	render_color = modulated;
}

void SkyWeatherColorBlocks::snap(const std::array<uint32_t, kCount> &packed_colors) {
	skyfog.snap(packed_colors[0]);
	ceiling.snap(packed_colors[1]);
	cloud.snap(packed_colors[2]);
	floor.snap(packed_colors[3]);
	skybase.snap(packed_colors[4]);
	skybright.snap(packed_colors[5]);
	skyhighlight.snap(packed_colors[6]);
	cloudbase.snap(packed_colors[7]);
	cloudhighlight.snap(packed_colors[8]);
	cloudedge.snap(packed_colors[9]);
}

void SkyWeatherColorBlocks::set_targets(const std::array<uint32_t, kCount> &packed_colors) {
	skyfog.target = packed_colors[0];
	ceiling.target = packed_colors[1];
	cloud.target = packed_colors[2];
	floor.target = packed_colors[3];
	skybase.target = packed_colors[4];
	skybright.target = packed_colors[5];
	skyhighlight.target = packed_colors[6];
	cloudbase.target = packed_colors[7];
	cloudhighlight.target = packed_colors[8];
	cloudedge.target = packed_colors[9];
}

void SkyWeatherColorBlocks::set_skyfog_additive(uint32_t packed_additive) {
	skyfog.additive = packed_additive;
}

void SkyWeatherColorBlocks::tick_skyfog(uint32_t modulator_packed, int rain_intensity) {
	skyfog.tick(modulator_packed, rain_intensity);
}

void SkyWeatherColorBlocks::tick_statics(uint32_t modulator_packed, int rain_intensity) {
	ceiling.tick(modulator_packed, rain_intensity);
	cloud.tick(modulator_packed, rain_intensity);
	floor.tick(modulator_packed, rain_intensity);
}

void SkyWeatherColorBlocks::tick_dome(uint32_t modulator_packed, int rain_intensity) {
	skybase.tick(modulator_packed, rain_intensity);
	skybright.tick(modulator_packed, rain_intensity);
	skyhighlight.tick(modulator_packed, rain_intensity);
	cloudbase.tick(modulator_packed, rain_intensity);
	cloudhighlight.tick(modulator_packed, rain_intensity);
	cloudedge.tick(modulator_packed, rain_intensity);
}

// ---------------------------------------------------------------------------
// Cloud scroll

void CloudScrollState::tick(int rate_target) {
	// [orig: Environment_UpdateWeatherTick — rate ramp @ 0x57eecc,
	//  accumulators @ 0x57f1a5..0x57f1d1]. rate/3 is the original's idiv:
	//  truncation toward zero.
	rate = smooth_eighth(rate, rate_target);
	acc_l1_v += rate;
	acc_l1_u += rate;
	acc_l2_v += rate - rate / 3;
	acc_l2_u += rate + rate / 3;
}

CloudUvOffsets cloud_scroll_uv_offsets(const CloudScrollState &scroll,
                                       float cam_x, float cam_z) {
	// [orig: render_skybox @ 0x5791de..0x579260] — the witnessed texture-
	// transform translations in the render basis: the camera term is
	// +cam/4096 on both axes (16.16 camera / 2^28|29), the accumulator term
	// is NEGATIVE on U and positive on V.
	CloudUvOffsets out;
	out.u1 = static_cast<float>(cam_x * (1.0 / 4096.0) -
	                            scroll.acc_l1_u * kCloudUvScaleLayer1);
	out.v1 = static_cast<float>(cam_z * (1.0 / 4096.0) +
	                            scroll.acc_l1_v * kCloudUvScaleLayer1);
	out.u2 = static_cast<float>(cam_x * (1.0 / 8192.0) -
	                            scroll.acc_l2_u * kCloudUvScaleLayer2);
	out.v2 = static_cast<float>(cam_z * (1.0 / 8192.0) +
	                            scroll.acc_l2_v * kCloudUvScaleLayer2);
	return out;
}

float cloud_uv_rate_per_second(const CloudScrollState &scroll) {
	// 62 ticks of the current rate through the layer-1 2^-28 UV scale.
	return static_cast<float>(scroll.rate * 62.0 * kCloudUvScaleLayer1);
}

// ---------------------------------------------------------------------------
// Sky dome mesh

SkyDomeMesh build_sky_dome_mesh(float sky_height) {
	// [orig: build_sky_dome_mesh @ 0x578db0] — structural translation; see the
	// header block for the full witness map. Doubles seeded from the binary's
	// float32 literals (.rdata @ 0x7d75c4..0x7d75e0), outputs stored float32
	// like the D3D vertex buffer.
	constexpr int kRows = 21;
	constexpr int kCols = 21;
	const double kRowStep = static_cast<double>(51.2f);       // @ 0x7d75d4
	const double kThetaStep = static_cast<double>(0.31415927f); // pi/10 @ 0x7d75c4
	const double kUv1Scale = static_cast<double>(0.003125f);  // 1/320 @ 0x7d75d0
	const double kUv2Scale = 0.00146484375;                   // 3/2048 @ 0x7d75cc
	const double kSphereRadiusSq = static_cast<double>(9437184.0f); // 3072^2 @ 0x7d75c8
	const double sqrt_base = std::sqrt(8388608.0);            // sqrt(2^23) @ 0x7d75e0

	SkyDomeMesh mesh;
	mesh.positions.reserve(kSkyDomeVertices * 3);
	mesh.normals.reserve(kSkyDomeVertices * 3);
	mesh.uv1.reserve(kSkyDomeVertices * 2);
	mesh.uv2.reserve(kSkyDomeVertices * 2);
	mesh.indices.reserve(kSkyDomeTriangles * 3);

	// Index buffer first, like the original: per quad (i, i+22, i+21) then
	// (i, i+1, i+22) [orig: @ 0x578e00..0x578e86].
	for (int row = 0; row < kRows - 1; ++row) {
		const int base = row * kCols;
		for (int col = 0; col < kCols - 1; ++col) {
			const int i = base + col;
			mesh.indices.push_back(i);
			mesh.indices.push_back(i + kCols + 1);
			mesh.indices.push_back(i + kCols);
			mesh.indices.push_back(i);
			mesh.indices.push_back(i + 1);
			mesh.indices.push_back(i + kCols + 1);
		}
	}

	// v14 = skyHeight / (3072 - sqrt(2^23)) [orig: @ 0x578ed4]; the Y scale is
	// baked, x/z stay at the 1024-unit rim.
	const double v14 = static_cast<double>(sky_height) / (3072.0 - sqrt_base);
	const double inv_y_scale_sq = 1.0 / (v14 * v14);
	for (int row = 0; row < kRows; ++row) {
		const double radius = row * kRowStep;
		const double y_unscaled = std::sqrt(kSphereRadiusSq - radius * radius) - sqrt_base;
		const double y_scaled = v14 * y_unscaled;
		for (int col = 0; col < kCols; ++col) {
			const double theta = col * kThetaStep;
			const double x = std::sin(theta) * row * kRowStep;
			const double z = std::cos(theta) * row * kRowStep;
			mesh.positions.push_back(static_cast<float>(x));
			mesh.positions.push_back(static_cast<float>(y_scaled));
			mesh.positions.push_back(static_cast<float>(z));
			mesh.uv1.push_back(static_cast<float>(x * kUv1Scale));
			mesh.uv1.push_back(static_cast<float>(z * kUv1Scale));
			mesh.uv2.push_back(static_cast<float>(x * kUv2Scale));
			mesh.uv2.push_back(static_cast<float>(z * kUv2Scale));

			// normalize(x, y_scaled / v14^2, z) [orig: @ 0x578fbb..0x579023];
			// zero length degenerates to (0,0,0) [orig: @ 0x578fd8].
			const double ny_in = y_scaled * inv_y_scale_sq;
			const double length = std::sqrt(z * z + ny_in * ny_in + x * x);
			if (length == 0.0) {
				mesh.normals.push_back(0.0f);
				mesh.normals.push_back(0.0f);
				mesh.normals.push_back(0.0f);
			} else {
				const double inv_length = 1.0 / length;
				mesh.normals.push_back(static_cast<float>(x * inv_length));
				mesh.normals.push_back(static_cast<float>(ny_in * inv_length));
				mesh.normals.push_back(static_cast<float>(z * inv_length));
			}
		}
	}
	return mesh;
}

// ---------------------------------------------------------------------------
// Water surface

uint32_t water_noise_prng_step(uint32_t state) {
	// [orig: PRNG_Next16 @ 0x6130a0] — rol4(state + rol11(state)) ^ 1.
	const uint32_t rolled11 = (state << 11) | (state >> 21);
	const uint32_t sum = state + rolled11;
	return (((sum << 4) | (sum >> 28)) ^ 1u);
}

WaterNoiseTables water_init_noise_tables() {
	// [orig: Water_InitNoiseFieldAndSineLut @ 0x5c01a0] — structural
	// translation; the reimpl seeds the PRNG from the boot state 0.
	WaterNoiseTables tables{};

	int32_t grid[kWaterNoiseSize * kWaterNoiseSize];
	uint32_t prng_state = 0;
	for (int i = 0; i < kWaterNoiseSize * kWaterNoiseSize; ++i) {
		prng_state = water_noise_prng_step(prng_state);
		grid[i] = 2 * static_cast<int32_t>(prng_state & 0xFFFFu) - 0x10000;
	}

	int32_t min_value = 0x40000000;
	int32_t max_value = -0x40000000;
	for (int i = 0; i < kWaterNoiseSize * kWaterNoiseSize; ++i) {
		min_value = std::min(min_value, grid[i]);
		max_value = std::max(max_value, grid[i]);
	}
	// range + range >> 8 keeps the normalized bytes strictly below 256
	// [orig: @ 0x5c0297].
	const int32_t range = max_value - min_value;
	const int32_t range_scaled = range + (range >> 8);
	for (int i = 0; i < kWaterNoiseSize * kWaterNoiseSize; ++i) {
		tables.field[i] = static_cast<uint8_t>(
				((grid[i] - min_value) << 8) / range_scaled);
	}

	// 128 + 64*sin: the original computes trunc(sin(i * 2pi/256) * -64) at init
	// and stores 0x80 - value [orig: @ 0x5c0308..0x5c0334; step float 2pi/256,
	// amplitude float -64] — x87 fsin, one deterministic instance. A runtime
	// std::sin build forks per libm at the trunc boundaries (macOS 26 images
	// flip non-landmark bytes and every downstream noise pixel with them), so
	// the LUT is the committed deterministic instance the parity pins were
	// generated from (formula-identical on MSVC/UCRT x64; env #35 in
	// docs/env/env-tod-re.md).
	static const uint8_t kSineLut[256] = {
		0x80, 0x81, 0x83, 0x84, 0x86, 0x87, 0x89, 0x8A, 0x8C, 0x8E, 0x8F, 0x91, 0x92, 0x94, 0x95, 0x97,
		0x98, 0x99, 0x9B, 0x9C, 0x9E, 0x9F, 0xA0, 0xA2, 0xA3, 0xA4, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAC,
		0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB6, 0xB7, 0xB8, 0xB9, 0xB9, 0xBA,
		0xBB, 0xBB, 0xBC, 0xBC, 0xBD, 0xBD, 0xBE, 0xBE, 0xBE, 0xBF, 0xBF, 0xBF, 0xBF, 0xBF, 0xBF, 0xBF,
		0xBF, 0xBF, 0xBF, 0xBF, 0xBF, 0xBF, 0xBF, 0xBF, 0xBE, 0xBE, 0xBE, 0xBD, 0xBD, 0xBC, 0xBC, 0xBB,
		0xBB, 0xBA, 0xB9, 0xB9, 0xB8, 0xB7, 0xB6, 0xB6, 0xB5, 0xB4, 0xB3, 0xB2, 0xB1, 0xB0, 0xAF, 0xAE,
		0xAD, 0xAC, 0xAA, 0xA9, 0xA8, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x99,
		0x98, 0x97, 0x95, 0x94, 0x92, 0x91, 0x8F, 0x8E, 0x8C, 0x8A, 0x89, 0x87, 0x86, 0x84, 0x83, 0x81,
		0x80, 0x7F, 0x7D, 0x7C, 0x7A, 0x79, 0x77, 0x76, 0x74, 0x72, 0x71, 0x6F, 0x6E, 0x6C, 0x6B, 0x69,
		0x68, 0x67, 0x65, 0x64, 0x62, 0x61, 0x60, 0x5E, 0x5D, 0x5C, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x54,
		0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4C, 0x4B, 0x4A, 0x4A, 0x49, 0x48, 0x47, 0x47, 0x46,
		0x45, 0x45, 0x44, 0x44, 0x43, 0x43, 0x42, 0x42, 0x42, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
		0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x42, 0x42, 0x42, 0x43, 0x43, 0x44, 0x44, 0x45,
		0x45, 0x46, 0x47, 0x47, 0x48, 0x49, 0x4A, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52,
		0x53, 0x54, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5C, 0x5D, 0x5E, 0x60, 0x61, 0x62, 0x64, 0x65, 0x67,
		0x68, 0x69, 0x6B, 0x6C, 0x6E, 0x6F, 0x71, 0x72, 0x74, 0x76, 0x77, 0x79, 0x7A, 0x7C, 0x7D, 0x7F,
	};
	std::copy(std::begin(kSineLut), std::end(kSineLut), tables.sine_lut);
	return tables;
}

void water_noise_color_pixels(uint32_t *out_pixels, const WaterNoiseTables &tables,
                              uint32_t frame_counter) {
	// [orig: Water_GenerateNoiseTextures @ 0x5c0360, passes 1+2].
	uint8_t animated[kWaterNoiseSize * kWaterNoiseSize];
	for (int i = 0; i < kWaterNoiseSize * kWaterNoiseSize; ++i) {
		const uint8_t source = tables.field[i];
		// Two speed classes: odd bytes advance at twice the counter rate.
		const uint8_t index = static_cast<uint8_t>(
				source + (frame_counter << (source & 1)));
		animated[i] = tables.sine_lut[index];
	}

	// Toroidal 9-tap kernel: 3x the four corners + 4x the cross (center +
	// 4-neighborhood), >> 5 [orig: @ 0x5c04b7..0x5c0552]. Written with
	// explicit wrapped row pointers + masked columns: the earlier
	// lambda-indexed form was miscompiled by the macos-26 AppleClang
	// autovectorizer (wrap-row taps went wrong for a band of row-0 pixels
	// while the same bytes summed correctly — caught by env_render_unit's
	// landmark pins; the math here is unchanged).
	for (int row = 0; row < kWaterNoiseSize; ++row) {
		const uint8_t *row_up = animated + (((row - 1) & 0x7F) << 7);
		const uint8_t *row_mid = animated + (row << 7);
		const uint8_t *row_down = animated + (((row + 1) & 0x7F) << 7);
		for (int col = 0; col < kWaterNoiseSize; ++col) {
			const int col_left = (col - 1) & 0x7F;
			const int col_right = (col + 1) & 0x7F;
			const int kernel =
					(3 * (row_up[col_left] + row_up[col_right] +
					      row_down[col_left] + row_down[col_right]) +
					 4 * (row_mid[col] + row_up[col] + row_down[col] +
					      row_mid[col_left] + row_mid[col_right])) >> 5;
			int intensity = 128 - std::abs(kernel - 128);
			if (intensity < 0) {
				intensity = 0;
			}
			int alpha_inv = (intensity * intensity) >> 9;
			if (alpha_inv < 0) {
				alpha_inv = 0;
			}
			out_pixels[(row << 7) + col] =
					(0x10101u * static_cast<uint32_t>(intensity)) |
					(static_cast<uint32_t>(255 - alpha_inv) << 24);
		}
	}
}

void water_noise_normal_pixels(uint32_t *out_pixels, const uint32_t *color_pixels) {
	// [orig: Water_GenerateNoiseTextures @ 0x5c07c2..0x5c087d] — the MMX
	// psubsb/paddsb/paddb chain on the intensity (blue) bytes, rows and
	// columns wrapping toroidally.
	const auto intensity = [color_pixels](int row, int col) -> int {
		return static_cast<int>(color_pixels[((row & 0x7F) << 7) + (col & 0x7F)] & 0xFFu);
	};
	const auto sat8 = [](int value) -> int {
		return std::max(-128, std::min(127, value));
	};
	for (int row = 0; row < kWaterNoiseSize; ++row) {
		for (int col = 0; col < kWaterNoiseSize; ++col) {
			const int center = intensity(row, col);
			// psubsb on the +0x80-biased bytes == signed-saturated byte
			// subtraction of the raw intensities.
			const int diff_row = sat8(sat8(center - intensity(row - 1, col)) * 2);
			const int diff_col = sat8(sat8(center - intensity(row, col - 1)) * 2);
			// paddb (wrapping) with the bias 0x008080FF: B=0xFF, G/R biased
			// +0x80, A=0.
			const uint32_t red = static_cast<uint32_t>((diff_row + 0x80) & 0xFF);
			const uint32_t green = static_cast<uint32_t>((diff_col + 0x80) & 0xFF);
			out_pixels[(row << 7) + col] = 0xFFu | (green << 8) | (red << 16);
		}
	}
}

WaterUvState water_uv_state(const CloudScrollState &scroll, float cam_x, float cam_z,
                            float fog_distance_world) {
	// [orig: render_water_surface @ 0x5c3348..0x5c33db].
	WaterUvState state;
	// w = the INTEGER part of the (smoothed) fog distance — the original
	// reads the 16-bit word above the 16.16 fraction.
	const double w = static_cast<double>(static_cast<int16_t>(fog_distance_world));
	const double v = w / (w - 0.2);
	state.scale = static_cast<float>(v * static_cast<double>(0.99996948f));
	state.bias = static_cast<float>(0.2 * v * static_cast<double>(0.99996948f));
	// Layer-1 cloud accumulators + the 32x camera term; engine axes
	// (camX_eng = render z, camY_eng = -render x), sums wrap as uint32 like
	// the original.
	const uint32_t cam_x_eng = static_cast<uint32_t>(static_cast<int64_t>(
			static_cast<double>(cam_z) * 65536.0));
	const uint32_t cam_y_eng = static_cast<uint32_t>(-static_cast<int64_t>(
			static_cast<double>(cam_x) * 65536.0));
	state.offset_u = static_cast<float>(
			static_cast<double>(static_cast<uint32_t>(scroll.acc_l1_v + 32u * cam_x_eng)) *
			kCloudUvScaleLayer1);
	state.offset_v = static_cast<float>(
			static_cast<double>(static_cast<uint32_t>(scroll.acc_l1_u - 32u * cam_y_eng)) *
			kCloudUvScaleLayer1);
	return state;
}

// ---------------------------------------------------------------------------
// Water strip tessellation (env #29)

namespace {

constexpr float kWaterFixedToFloat = 1.52587890625e-05f; // 2^-16 [orig: flt_7C3310]

// _ftol2_sse truncation. The retail-unreachable overflow range saturates
// instead of producing x86's 0x80000000 indefinite, so the C++ stays defined;
// every observable strip output is identical (the callers clamp right after).
int32_t ftol_trunc(double value) {
	if (value >= 2147483647.0) {
		return 2147483647;
	}
	if (value <= -2147483648.0) {
		return INT32_MIN;
	}
	return static_cast<int32_t>(value);
}

// world -> viewport pixels through the view then projection matrix, both in
// the originals' row-vector convention (out = v * M; translation in row 3)
// [orig: Math_TransformPointByMatrix4x4 @ 0x40cf20 ->
// Math_TransformPoint4ByMatrix4x4_Float @ 0x612e80, screen mapping
// s = center +- clip/(2w) * (max - min) @ 0x5c0cf1..0x5c0d25].
void water_project_point(const WaterStripView &view, const float world[3], float out_xy[2]) {
	const float *m = view.view;
	const float vx = m[0] * world[0] + m[4] * world[1] + m[8] * world[2] + m[12];
	const float vy = m[1] * world[0] + m[5] * world[1] + m[9] * world[2] + m[13];
	const float vz = m[2] * world[0] + m[6] * world[1] + m[10] * world[2] + m[14];
	const float *p = view.proj;
	const float cx = p[0] * vx + p[4] * vy + p[8] * vz + p[12];
	const float cy = p[1] * vx + p[5] * vy + p[9] * vz + p[13];
	const float cw = p[3] * vx + p[7] * vy + p[11] * vz + p[15];
	const double inv_2w = 1.0 / (static_cast<double>(cw) + static_cast<double>(cw));
	out_xy[0] = static_cast<float>(
			view.vp_center_x + cx * inv_2w * (view.vp_max_x - view.vp_min_x));
	out_xy[1] = static_cast<float>(
			view.vp_center_y - cy * inv_2w * (view.vp_max_y - view.vp_min_y));
}

} // namespace

void water_project_plane_to_screen(const WaterStripView &view, int32_t plane_height_fp,
                                   WaterScreenBlock &out) {
	// [orig: terrain_project_sector_to_screen @ 0x5c0bf0] — structural
	// translation; the camera block converts fild * 2^-16 (@ 0x5c0c08).
	const float cam_x = view.cam_x_fp * kWaterFixedToFloat;
	const float cam_z = view.cam_z_fp * kWaterFixedToFloat;
	const float plane_y = plane_height_fp * kWaterFixedToFloat;

	// The horizontal forward: view-matrix column 2's x/z, normalized in the
	// ground plane; a zero-length pair stays raw [orig: @ 0x5c0c19..0x5c0c56].
	float fwd_x = view.view[2];
	float fwd_z = view.view[10];
	const float fwd_len = std::sqrt(fwd_z * fwd_z + fwd_x * fwd_x);
	if (fwd_len != 0.0f) {
		const float inv_len = 1.0f / fwd_len;
		fwd_x *= inv_len;
		fwd_z *= inv_len;
	}

	// Origin: the plane point at camera + horizontal-forward * 2000
	// [orig: @ 0x5c0c7c..0x5c0d25].
	float p0[3] = {cam_x + fwd_x * 2000.0f, plane_y, cam_z + fwd_z * 2000.0f};
	water_project_point(view, p0, out.origin);

	// Row delta: a 1000-unit horizontal RIGHT step (view column 0's x/z,
	// unnormalized), projected relative to the origin; dy forced to 1e-6
	// when 0 [orig: @ 0x5c0d3c..0x5c0deb].
	const float p1[3] = {p0[0] + 1000.0f * view.view[0], plane_y,
	                     p0[2] + 1000.0f * view.view[8]};
	float s1[2];
	water_project_point(view, p1, s1);
	out.row_delta[0] = s1[0] - out.origin[0];
	out.row_delta[1] = s1[1] - out.origin[1];
	if (out.row_delta[1] == 0.0f) {
		out.row_delta[1] = 0.000001f;
	}

	// The 1000-unit reference point [orig: @ 0x5c0e0f..0x5c0e9b].
	const float p2[3] = {cam_x + fwd_x * 1000.0f, plane_y, cam_z + fwd_z * 1000.0f};
	water_project_point(view, p2, out.ref_point);

	// March direction = normalize(ref - origin); degenerate -> (0, 1)
	// [orig: @ 0x5c0ea1..0x5c0ee8].
	const float march_dx = out.ref_point[0] - out.origin[0];
	const float march_dy = out.ref_point[1] - out.origin[1];
	const float march_len = std::sqrt(march_dy * march_dy + march_dx * march_dx);
	if (march_len == 0.0f) {
		out.march_dir[0] = 0.0f;
		out.march_dir[1] = 1.0f;
	} else {
		const float inv_len = 1.0f / march_len;
		out.march_dir[0] = march_dx * inv_len;
		out.march_dir[1] = march_dy * inv_len;
	}

	// Visibility: in-viewport, else the origin row's line crossing, else the
	// same-side halfplane pair with the entry-edge origin clamp
	// [orig: @ 0x5c0eea..0x5c1029].
	out.visible = 0;
	if (static_cast<float>(view.vp_min_x) <= out.origin[0] &&
	    static_cast<float>(view.vp_max_x) >= out.origin[0] &&
	    static_cast<float>(view.vp_min_y) <= out.origin[1] &&
	    static_cast<float>(view.vp_max_y) >= out.origin[1]) {
		out.visible = 1;
	} else {
		WaterRowClip clip;
		water_clip_row_to_viewport(view, out.origin[0], out.origin[1],
		                           out.row_delta[0] / out.row_delta[1], clip);
		out.visible = clip.crossed ? 1 : 0;
	}
	out.origin_row_visible = out.visible;
	if (!out.visible) {
		// The row line through the origin with normal = march_dir: visible
		// when the reference point and the viewport center sit strictly on
		// the same side [orig: @ 0x5c0f7b..0x5c0ffd].
		const float nx = out.march_dir[0];
		const float ny = out.march_dir[1];
		const float d = -(out.origin[1] * ny + out.origin[0] * nx);
		const float ref_side = out.ref_point[0] * nx + out.ref_point[1] * ny + d;
		const float center_side = static_cast<float>(view.vp_center_x) * nx +
				static_cast<float>(view.vp_center_y) * ny + d;
		if ((ref_side > 0.0f && center_side > 0.0f) ||
		    (ref_side < 0.0f && center_side < 0.0f)) {
			out.visible = 1;
			// Marching in from off-screen: the origin clamps onto the edges
			// the march will enter through [orig: @ 0x5c0fd1..0x5c1023]. The
			// witnessed depth-W perspective keeps its exact >= edge choice.
			// Under constant-W orthographic projection, however, a zero march
			// component leaves that coordinate unconstrained; anchoring it at an
			// outer edge combines with the literal row-dy guard to collapse the
			// first row. Use the viewport center only for that reimpl extension.
			const bool constant_clip_w = view.proj[11] == 0.0f && view.proj[15] != 0.0f;
			if (constant_clip_w) {
				// Start one pixel inside the entering edge. At the exact edge,
				// the witnessed 1e-6 row-dy guard can round the nominal endpoint
				// just outside and collapse an otherwise full orthographic row.
				out.origin[1] = ny == 0.0f ? static_cast<float>(view.vp_center_y)
						: (ny < 0.0f ? static_cast<float>(view.vp_max_y)
						             : static_cast<float>(view.vp_min_y + 1));
				out.origin[0] = nx == 0.0f ? static_cast<float>(view.vp_center_x)
						: (nx < 0.0f ? static_cast<float>(view.vp_max_x)
						             : static_cast<float>(view.vp_min_x + 1));
			} else {
				out.origin[1] = (0.0f >= ny) ? static_cast<float>(view.vp_max_y) + 1.0f
				                                 : static_cast<float>(view.vp_min_y);
				out.origin[0] = (0.0f >= nx) ? static_cast<float>(view.vp_max_x) + 1.0f
				                                 : static_cast<float>(view.vp_min_x);
			}
		}
	}
}

void water_clip_row_to_viewport(const WaterStripView &view, float x0, float y0,
                                float dx_over_dy, WaterRowClip &out) {
	// [orig: clip_line_to_viewport @ 0x5c0a30] — endpoints seed at the left
	// and right rect edges through inv = dy/dx, then clamp vertically through
	// the dx/dy slope; the rect is [min_x, max_x + 1] x [min_y, max_y + 1].
	const float left = static_cast<float>(view.vp_min_x);
	const float right = static_cast<float>(view.vp_max_x + 1);
	const float top = static_cast<float>(view.vp_min_y);
	const float bottom = static_cast<float>(view.vp_max_y + 1);
	const float inv_slope = 1.0f / dx_over_dy;

	out.left[0] = left;
	out.left[1] = y0 - (x0 - left) * inv_slope;
	out.right[0] = right;
	out.right[1] = y0 - inv_slope * (x0 - right);

	if (top <= out.left[1]) {
		if (bottom < out.left[1]) {
			out.left[0] = x0 - (y0 - bottom) * dx_over_dy;
			out.left[1] = bottom;
		}
	} else {
		out.left[0] = x0 - (y0 - top) * dx_over_dy;
		out.left[1] = top;
	}
	if (top <= out.right[1]) {
		if (bottom < out.right[1]) {
			out.right[0] = x0 - dx_over_dy * (y0 - bottom);
			out.right[1] = bottom;
		}
	} else {
		out.right[0] = x0 - dx_over_dy * (y0 - top);
		out.right[1] = top;
	}

	out.crossed = !(left > out.left[0] || right < out.left[0]) &&
			!(top > out.left[1] || bottom < out.left[1]) &&
			!(left > out.right[0] || right < out.right[0]) &&
			!(top > out.right[1] || bottom < out.right[1]);
}

int water_strip_stride(float row_rhw) {
	// [orig: @ 0x5c30c7..0x5c30eb — ftol(rhw * 500) clamped 2..9;
	// flt_7D6FB4 = 500.0]
	const int steps = ftol_trunc(static_cast<double>(row_rhw) * 500.0);
	if (steps < 2) {
		return 2;
	}
	if (steps > 9) {
		return 9;
	}
	return steps;
}

float water_strip_depth(float view_depth, float uv_scale, float uv_bias) {
	// [orig: @ 0x5c2bfd..0x5c2c4a] — rhw first, then z = (t*scale - bias)*rhw
	// against the witnessed clamp pair (flt_7C4658 upper / flt_7DBF7C lower).
	const float rhw = 1.0f / view_depth;
	float z = (view_depth * uv_scale - uv_bias) * rhw;
	if (z > kWaterStripDepthMax) {
		z = kWaterStripDepthMax;
	}
	if (z < kWaterStripDepthMin) {
		z = kWaterStripDepthMin;
	}
	return z;
}

WaterRowColors water_strip_row_colors(float row_view_depth, const float right_delta[3],
                                      int32_t fog_end_fp, float water_murk,
                                      uint32_t water_color_lit_packed,
                                      bool underwater_view, bool nightvision) {
	// [orig: render_water_strip_detailed @ 0x5c2d3f..0x5c2ef6] — the header
	// block maps the chain; every constant below is the cited literal.
	float base = 1.0f - water_murk; // [orig: fsub Env_WaterMurk @ 0x5c2d46]
	if (underwater_view) {
		base = 1.0f; // the murk term is skipped [orig: @ 0x5c2d4c..0x5c2d50]
	}
	if (nightvision) {
		base = 0.1f; // flt_7C69F4 [orig: @ 0x5c2d5a]
	}
	const float k = 0.8f * base + 0.2f;  // flt_7C6F9C / flt_7C3340 [orig: @ 0x5c2d60..0x5c2d7a]
	const float bright_far = 38.4f * k;  // flt_7DBFA8 [orig: @ 0x5c2d80]
	const float bright_near = 192.0f * k; // flt_7DBFA4 [orig: @ 0x5c2d93]
	const float alpha_lo = 0.0f * base;  // flt_7C3284 = 0.0 (retail multiplies zero) [orig: @ 0x5c2da4]
	const float alpha_hi = 229.5f * base; // flt_7DBFA0 [orig: @ 0x5c2daf]
	const float one_minus_base = 1.0f - base;    // [orig: @ 0x5c2db8]
	const float spec_lo = 128.0f * one_minus_base; // flt_7C461C [orig: @ 0x5c2dbe]
	const float spec_hi = 255.0f * one_minus_base; // flt_7CA29C [orig: @ 0x5c2dc9]

	// The sine of the right-edge ray's depression angle: |dy| / |delta|
	// [orig: @ 0x5c2dd2..0x5c2def].
	const double horiz_sq = static_cast<double>(right_delta[0]) * right_delta[0] +
			static_cast<double>(right_delta[2]) * right_delta[2];
	const double dist = std::sqrt(
			horiz_sq + static_cast<double>(right_delta[1]) * right_delta[1]);
	const double sin_angle = std::fabs(static_cast<double>(right_delta[1])) / dist;

	int brightness;
	int diffuse_alpha;
	int dist_alpha;
	if (underwater_view) {
		// Solid white diffuse; LINEAR distance falloff (no square). Both
		// tiers' distance term multiplies dbl_7DBF98 = 2^24 (= 256 per world
		// unit against the 16.16 fog end); the doc's "x255 <-> x229.5" swap
		// is the LOW tier's dbl_7DBF70 @ 0x5c244b. [orig: @ 0x5c2df3..0x5c2e20]
		brightness = 255;
		diffuse_alpha = 255;
		const int a = ftol_trunc(
				static_cast<double>(row_view_depth) * 16777216.0 / fog_end_fp);
		dist_alpha = clamp_int(255 - a, 0, 255);
	} else {
		// [orig: @ 0x5c2e22..0x5c2e9d] — the two lerps ftol-truncate; the
		// /255 divisions are the 0x80808081 magic (exact truncating idiv).
		const int alpha_term = ftol_trunc(alpha_lo + (alpha_hi - alpha_lo) * sin_angle);
		brightness = ftol_trunc(bright_far + (bright_near - bright_far) * (1.0 - sin_angle));
		const int a = clamp_int(
				ftol_trunc(static_cast<double>(row_view_depth) * 16777216.0 / fog_end_fp),
				0, 255);
		dist_alpha = 255 - a * a / 255;
		diffuse_alpha = alpha_term * dist_alpha / 255;
	}

	WaterRowColors colors;
	colors.diffuse = (static_cast<uint32_t>(diffuse_alpha) << 24) |
			(0x10101u * static_cast<uint32_t>(brightness)); // [orig: @ 0x5c2e9f..0x5c2eab]
	if (nightvision) {
		colors.specular = static_cast<uint32_t>(dist_alpha) << 24; // [orig: @ 0x5c2ef8]
	} else {
		// [orig: @ 0x5c2eb5..0x5c2ef4] — WaterColorLit bytes * term >> 8
		// under the distance alpha in the top byte.
		const int term = ftol_trunc(spec_lo + (spec_hi - spec_lo) * (1.0 - sin_angle));
		const uint32_t r = (((water_color_lit_packed >> 16) & 0xFFu) * term) >> 8;
		const uint32_t g = (((water_color_lit_packed >> 8) & 0xFFu) * term) >> 8;
		const uint32_t b = ((water_color_lit_packed & 0xFFu) * term) >> 8;
		colors.specular =
				(static_cast<uint32_t>(dist_alpha) << 24) | (r << 16) | (g << 8) | b;
	}
	return colors;
}

int water_build_strip_rows(const WaterStripView &view, const WaterStripParams &params,
                           WaterStripRows &out) {
	// [orig: render_water_strip_detailed @ 0x5c27d0] — structural translation
	// of the march loop; the device/VB setup and the batch submits stay with
	// the embedder (water_strip_batches expresses the submit shape). x87
	// intermediates approximated as double, stored float32 like the original
	// stack spills (the sky-dome port's convention).
	out.screen_pos.clear();
	out.depth.clear();
	out.rhw.clear();
	out.diffuse.clear();
	out.specular.clear();
	out.uv0.clear();
	out.t1.clear();
	out.t2.clear();

	WaterScreenBlock block;
	water_project_plane_to_screen(view, params.plane_height_fp, block);
	if (!block.visible) { // [orig: @ 0x5c28c6]
		return 0;
	}

	// Pass-constant state [orig: @ 0x5c2822..0x5c28ac]. The half extents,
	// centers and projection reciprocals are re-derived per row in retail
	// (@ 0x5c29ce..0x5c2a32) with identical values — hoisted here.
	const float cam_x = view.cam_x_fp * kWaterFixedToFloat;
	const float cam_y = view.cam_y_fp * kWaterFixedToFloat;
	const float cam_z = view.cam_z_fp * kWaterFixedToFloat;
	const float plane_y = params.plane_height_fp * kWaterFixedToFloat;
	const float width = static_cast<float>(view.vp_max_x - view.vp_min_x);
	const float height = static_cast<float>(view.vp_max_y - view.vp_min_y);
	const float inv_width = 1.0f / width;   // var_DC
	const float inv_height = 1.0f / height; // var_B8
	const float half_width = width * 0.5f;  // flt_7C3B94 = 0.5
	const float half_height = height * 0.5f;
	const float center_x = static_cast<float>(view.vp_center_x);
	const float center_y = static_cast<float>(view.vp_center_y);
	const float inv_m00 = 1.0f / view.proj[0]; // var_54 (mat @ 0x2721980)
	const float inv_m11 = 1.0f / view.proj[5]; // var_48 (flt_2721994)
	const float *inv = view.view_inv;

	const float slope = block.row_delta[0] / block.row_delta[1]; // var_6C [orig: @ 0x5c2918]
	float px = block.origin[0]; // var_8
	float py = block.origin[1]; // var_4
	double last_t = 1.0;        // var_64 [orig: fld1 @ 0x5c28a7]
	int stride = 4;             // var_1C [orig: @ 0x5c286d]
	int rows = 0;

	// Unprojects a clipped screen point to the plane. For Camera3D's projection
	// family, clip X/Y are diagonal plus a view-Z shear/translation and clip W
	// is `p11 * viewZ + p15`. Solving those two equations yields one view-space
	// line parameterized by view depth:
	//   origin = ((ndc*p15 - translation) / focal, ..., 0)
	//   direction = ((ndc*p11 - z_shear) / focal, ..., 1)
	// Centered perspective reduces exactly to the witnessed ray
	// (ndc/m00, ndc/m11, 1); orthographic instead gets a per-pixel origin and
	// parallel forward direction; off-center frusta retain their z shear.
	// The line is transformed through the inverse view rows, then intersected
	// with the water plane. A zero direction Y keeps the witnessed fallback
	// [orig: left @ 0x5c29ef..0x5c2ae8; right @ 0x5c2b08..0x5c2bce].
	const auto unproject = [&](float sx, float sy, double &t, double delta[3]) {
		const double ndc_x = (static_cast<double>(sx) - center_x) / half_width;
		const double ndc_y = -((static_cast<double>(sy) - center_y) / half_height);
		const double origin_vx = (ndc_x * view.proj[15] - view.proj[12]) * inv_m00;
		const double origin_vy = (ndc_y * view.proj[15] - view.proj[13]) * inv_m11;
		const double dir_vx = (ndc_x * view.proj[11] - view.proj[8]) * inv_m00;
		const double dir_vy = (ndc_y * view.proj[11] - view.proj[9]) * inv_m11;
		const double origin_x = cam_x + inv[0] * origin_vx + inv[4] * origin_vy;
		const double origin_y = cam_y + inv[1] * origin_vx + inv[5] * origin_vy;
		const double origin_z = cam_z + inv[2] * origin_vx + inv[6] * origin_vy;
		const double ray_x = inv[0] * dir_vx + inv[4] * dir_vy + inv[8];
		const double ray_y = inv[1] * dir_vx + inv[5] * dir_vy + inv[9];
		const double ray_z = inv[2] * dir_vx + inv[6] * dir_vy + inv[10];
		if (ray_y != 0.0) {
			// (originY - planeY) * (-1/rayY); originY == camY on
			// the witnessed perspective path [orig: flt_7D7C00 @ 0x5c2ac4].
			t = (origin_y - plane_y) * (-1.0 / ray_y);
			last_t = t;
			delta[0] = origin_x - cam_x + ray_x * t;
			delta[1] = origin_y - cam_y + ray_y * t;
			delta[2] = origin_z - cam_z + ray_z * t;
		} else {
			t = last_t;
			delta[0] = origin_x - cam_x + ray_x;
			delta[1] = origin_y - cam_y + ray_y;
			delta[2] = origin_z - cam_z + ray_z;
		}
	};
	// D3D transformed-vertex RHW is reciprocal CLIP W, not necessarily
	// reciprocal view depth. They coincide on the witnessed perspective path;
	// orthographic Camera3D projections instead carry constant clip W = 1.
	const auto reciprocal_clip_w = [&](double view_depth) {
		return static_cast<float>(1.0 /
				(view.proj[11] * view_depth + view.proj[15]));
	};

	WaterRowClip clip;
	for (;;) {
		water_clip_row_to_viewport(view, px, py, slope, clip);
		if (!clip.crossed) {
			// Hunt backward by single march steps (up to stride - 1) for the
			// last crossing row line [orig: @ 0x5c296d..0x5c29c3].
			if (stride <= 1) {
				break;
			}
			for (int hunt = 1;;) {
				px -= block.march_dir[0];
				py -= block.march_dir[1];
				water_clip_row_to_viewport(view, px, py, slope, clip);
				if (clip.crossed) {
					break;
				}
				if (++hunt >= stride) {
					break;
				}
			}
			if (!clip.crossed) {
				break;
			}
		}

		// --- One row: left / mid / right of the clipped span ---
		double t_left = 0.0;
		double t_right = 0.0;
		double delta_l[3];
		double delta_r[3];
		unproject(clip.left[0], clip.left[1], t_left, delta_l);
		const float u0_l = static_cast<float>((cam_x + delta_l[0]) * 0.03125); // flt_7DBFAC
		const float v0_l = static_cast<float>((cam_z + delta_l[2]) * 0.03125);
		unproject(clip.right[0], clip.right[1], t_right, delta_r);
		const float u0_r = static_cast<float>((cam_x + delta_r[0]) * 0.03125);
		const float v0_r = static_cast<float>((cam_z + delta_r[2]) * 0.03125);

		const float mid_x = (clip.right[0] + clip.left[0]) * 0.5f; // [orig: @ 0x5c2c4c]
		const float mid_y = (clip.right[1] + clip.left[1]) * 0.5f;
		const float u0_m = (u0_r + u0_l) * 0.5f;
		const float v0_m = (v0_r + v0_l) * 0.5f;
		const double t_mid = (t_left + t_right) * 0.5; // [orig: @ 0x5c2c7e]

		const float row_rhw = reciprocal_clip_w(t_left); // perspective: fst 1/t @ 0x5c2c06
		const float right_delta[3] = {static_cast<float>(delta_r[0]),
		                              static_cast<float>(delta_r[1]),
		                              static_cast<float>(delta_r[2])};
		const WaterRowColors colors = water_strip_row_colors(
				static_cast<float>(t_left), right_delta, view.fog_end_fp,
				params.water_murk, params.water_color_lit, params.underwater_view,
				params.nightvision);

		// Row-constant texm3x2 bump rows [orig: @ 0x5c2efd..0x5c2fcd]:
		// scale = min(rhw, 0.05); right row * -scale/2, forward row * -5*scale;
		// vbase = 1 - min(297*rhw + 0.15, 2)/256. The rows' world-Y products
		// are dead stores in retail and are not emitted.
		float bump = row_rhw;
		if (bump > 0.05f) { // flt_7C68E8
			bump = 0.05f;
		}
		const float right_scale = bump * -0.5f; // flt_7C59B0
		const float fwd_scale = -5.0f * bump;   // flt_7DBF94
		const float t1_x = view.cam_right[0] * right_scale;
		const float t1_y = view.cam_right[2] * right_scale;
		const float t2_x = view.cam_forward[0] * fwd_scale;
		const float t2_y = view.cam_forward[2] * fwd_scale;
		float q = 297.0f * row_rhw + 0.15f; // flt_7DBF68 / flt_7C6FA4
		if (q > 2.0f) {                     // flt_7C3B90
			q = 2.0f;
		}
		const float v_base = 1.0f - (q * 0.5f) * 0.0078125f; // flt_7C3DD4 = 1/128

		const float min_x = static_cast<float>(view.vp_min_x);
		const float min_y = static_cast<float>(view.vp_min_y);
		const float screen_x[3] = {clip.left[0], mid_x, clip.right[0]};
		const float screen_y[3] = {clip.left[1], mid_y, clip.right[1]};
		const double depth_t[3] = {t_left, t_mid, t_right};
		const float u0[3] = {u0_l, u0_m, u0_r};
		const float v0[3] = {v0_l, v0_m, v0_r};
		for (int i = 0; i < 3; ++i) {
			out.screen_pos.push_back(screen_x[i]);
			out.screen_pos.push_back(screen_y[i]);
			out.depth.push_back(water_strip_depth(static_cast<float>(depth_t[i]),
			                                      params.uv_scale, params.uv_bias));
			out.rhw.push_back(reciprocal_clip_w(depth_t[i]));
			out.diffuse.push_back(colors.diffuse);
			out.specular.push_back(colors.specular);
			out.uv0.push_back(u0[i]);
			out.uv0.push_back(v0[i]);
			out.t1.push_back(t1_x);
			out.t1.push_back(t1_y);
			out.t1.push_back((screen_x[i] - min_x) * inv_width); // [orig: @ 0x5c2fd0..]
			float t2_z = v_base - (screen_y[i] - min_y) * inv_height; // [orig: @ 0x5c301e..]
			if (params.underwater_view) {
				// The underwater pass samples the offscreen scene
				// upside-down [orig: @ 0x5c306f..0x5c3085].
				t2_z = 1.0f - t2_z;
			}
			out.t2.push_back(t2_x);
			out.t2.push_back(t2_y);
			out.t2.push_back(t2_z);
		}
		rows += 1;

		// Adaptive stride from this row's 1/w; the underwater pass never
		// re-derives (the boot 4 holds) [orig: @ 0x5c30c5..0x5c30eb].
		if (!params.underwater_view) {
			stride = water_strip_stride(row_rhw);
		}
		// March [orig: @ 0x5c30f2..0x5c3132] and the 1024-row cap
		// [orig: cmp 0x400 @ 0x5c312d].
		px += block.march_dir[0] * static_cast<float>(stride);
		py += block.march_dir[1] * static_cast<float>(stride);
		if (rows >= kWaterStripMaxRows) {
			break;
		}
	}
	return rows;
}

std::vector<WaterStripBatch> water_strip_batches(int row_count) {
	// [orig: @ 0x5c313f..0x5c329e] — <=5-row windows stepping 4 (1-row
	// overlap); windows under 2 rows draw nothing. Vertices lock 8n-10 per
	// window (@ 0x5c3195), drawn as a TRIANGLESTRIP of 8n-12 primitives
	// (DrawPrimitive @ 0x5c3209 passes vertex_count - 2).
	std::vector<WaterStripBatch> batches;
	if (row_count < 2) {
		return batches;
	}
	int start = 0;
	int end = row_count < 5 ? row_count : 5;
	while (start < row_count) {
		const int rows = end - start;
		if (rows >= 2) {
			WaterStripBatch batch;
			batch.first_row = start;
			batch.rows = rows;
			batch.vertex_count = 8 * rows - 10;
			batch.primitive_count = batch.vertex_count - 2;
			batches.push_back(batch);
		}
		start += 4;
		end = start + 5;
		if (end > row_count) {
			end = row_count;
		}
	}
	return batches;
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

int glare_brightness_step(int current, int target) {
	// [orig: render_skybox_sun_glow @ 0x5acf5d..0x5acf7f] — +-16 per frame
	// with a +-16 DEAD-BAND HOLD (the original never snaps onto the target;
	// the earlier port's snap+255-clamp was unwitnessed).
	if (current > target - 16) {
		if (current >= target + 16) {
			return current - 16;
		}
		return current;
	}
	return current + 16;
}

int celestial_sun_alpha_fixed(int overcast_blend_fixed, int sun_dim_fixed) {
	// [orig: render_celestial_bodies @ 0x5acbc1..0x5acbfa].
	const int64_t fold = static_cast<int64_t>(0x10000 - overcast_blend_fixed) *
			((0x640000 - sun_dim_fixed + 1) / 100);
	const int alpha = static_cast<int>((fold + 0x8000) >> 16);
	return clamp_int(alpha, 0, 0x10000);
}

int celestial_moon_alpha_fixed(float fog_distance_world, int overcast_blend_fixed,
                               bool fog_shader_path) {
	// [orig: render_celestial_bodies @ 0x5acc40..0x5acccd] — float chain off
	// the fog-distance INT word; result clamped 0..1 then scaled 0x10000.
	const double fog_int = static_cast<double>(static_cast<int16_t>(fog_distance_world));
	const double base = fog_shader_path
			? fog_int * static_cast<double>(0.0002f)
			: (fog_int - 400.0) * static_cast<double>(0.0016666667f);
	const double value = base * static_cast<double>(0x10000 - overcast_blend_fixed) *
			static_cast<double>(1.5258789e-05f);
	if (value <= 0.0) {
		return 0;
	}
	if (value >= 1.0) {
		return 0x10000;
	}
	return static_cast<int>(value * 65536.0);
}

GlareRayJitter glare_ray_jitter(uint32_t jitter_index) {
	// [orig: render_skybox_sun_glow @ 0x5ace3b..0x5ace61].
	GlareRayJitter jitter;
	jitter.offset_eng_y = ((jitter_index & 1u) ? 16.0f : -16.0f) +
			((jitter_index & 4u) ? 8.0f : -8.0f);
	jitter.offset_eng_z = (jitter_index & 2u) ? 16.0f : -16.0f;
	return jitter;
}

void glare_occlusion_tick(GlareOcclusionState &state, bool visible_a, bool visible_b,
                          float fog_distance_world) {
	// [orig: render_skybox_sun_glow @ 0x5acdfb..0x5acf7f] — two samples per
	// frame into the sliding window, then the dead-band brightness step
	// toward popcount * 32 * fog/1000 (truncating like the original ftol).
	state.jitter_index += 1;
	state.window = static_cast<uint8_t>((state.window >> 1) | (visible_a ? 0x80u : 0u));
	state.jitter_index += 1;
	state.window = static_cast<uint8_t>((state.window >> 1) | (visible_b ? 0x80u : 0u));

	int target32 = 0;
	for (int bit = 0; bit < 8; ++bit) {
		if (state.window & (1u << bit)) {
			target32 += 32;
		}
	}
	const double fog_factor = static_cast<double>(fog_distance_world) * 65536.0 *
			static_cast<double>(1.525878978725359e-08f); // flt_7DA0C4 = 1/65536000
	const int target = static_cast<int>(target32 * fog_factor);
	state.brightness = glare_brightness_step(state.brightness, target);
}

int glare_glow_alpha_fixed(int view_dot_fixed, int brightness, int overcast_blend_fixed,
                           int sun_dim_fixed) {
	// [orig: render_skybox_sun_glow @ 0x5acfb8..0x5ad0a9] — dot^4 / 2 in
	// 16.16, scaled by brightness >> 8, then the overcast x SunDim fold.
	int dot_factor = 0;
	if (view_dot_fixed > 0) {
		const int squared = static_cast<int>(
				(static_cast<int64_t>(view_dot_fixed) * view_dot_fixed + 0x8000) >> 16);
		dot_factor = static_cast<int>(
				(static_cast<int64_t>(squared) * squared + 0x8000) >> 16) >> 1;
	}
	const int scaled = (brightness * dot_factor) >> 8;
	const int64_t dim_fold = static_cast<int64_t>(scaled) *
			((0x640000 - sun_dim_fixed + 1) >> 8) / 25600;
	const int alpha = static_cast<int>(
			(static_cast<int64_t>(0x10000 - overcast_blend_fixed) * dim_fold + 0x8000) >> 16);
	return clamp_int(alpha, 0, 0x10000);
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

float iris_luminance(const Rgb &c) {
    // [orig: @ 0x5c7550] lum = 0.25*(r+b) + 0.5*g.
    return 0.25f * (c.r + c.b) + 0.5f * c.g;
}

int iris_gain(const Rgb &directional, const Rgb &sky, const Rgb &ground,
              float dir_x, float dir_y, float dir_z,
              float iris_center, float iris_percent) {
    // [orig: terrain_sector_compute_lighting @ 0x5c7550] exact curve.
    const float dir_lum = iris_luminance(directional);
    const float sky_lum = iris_luminance(sky);
    const float gnd_lum = iris_luminance(ground);
    const float vert_lum = dir_y * dir_lum + sky_lum;
    const float horiz_lum =
            std::sqrt(dir_x * dir_x + dir_z * dir_z) * dir_lum + 0.707f * (sky_lum + gnd_lum);
    float m = dir_lum;
    if (sky_lum > m) m = sky_lum;
    if (gnd_lum > m) m = gnd_lum;
    if (vert_lum > m) m = vert_lum;
    if (horiz_lum > m) m = horiz_lum;
    const float base = iris_center * 64.0f;
    // gain = 0.01 * (iris_percent * base/(2m) + (100 - iris_percent) * base).
    // m can be 0 (fully dark) — the base/(2m) term then diverges toward the
    // 255 clamp, matching the witnessed "rises toward 255 in darkness".
    float gain;
    if (m > 0.0f) {
        gain = 0.01f * (iris_percent * base / (2.0f * m) + (100.0f - iris_percent) * base);
    } else {
        gain = 255.0f; // the base/(2*0) limit is the clamp
    }
    const int g = static_cast<int>(gain);
    if (g < 0) return 0;
    if (g > 255) return 255;
    return g;
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
	// [orig: sample_terrain_colormap_tinted @ 0x606030] per channel
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

// ---------------------------------------------------------------------------
// Star field (env #33)

namespace {

inline uint32_t star_rotl32(uint32_t value, int count) {
	return (value << count) | (value >> (32 - count));
}

} // namespace

uint32_t star_prng_next(uint32_t &state) {
	// [orig: inlined at Star_GenerateInstanceTable @ 0x5ac850 and
	// render_star_field @ 0x5adb1a; standalone dead stub @ 0x5ac010]
	const uint32_t rolled = star_rotl32(state + star_rotl32(state, 11), 4) ^ 1u;
	state = rolled;
	return rolled & 0xFFFFu;
}

namespace {

// int(min(len, 2147418112.0f)) — the generator's ftol overflow guard
// [orig: flt_7C19E0 = 0x7FFF8000 as float].
int32_t star_length_int(double len) {
	const double kCeil = 2147418112.0;
	return static_cast<int32_t>(len < kCeil ? len : kCeil);
}

} // namespace

void generate_star_instances(StarInstance *out, uint32_t &prng_state) {
	// [orig: Star_GenerateInstanceTable @ 0x5ac850] — draw order per star:
	// offX, offY, offZ, billboard, mask, add; brightness untouched.
	for (int i = 0; i < kStarInstanceCount; ++i) {
		StarInstance &star = out[i];
		const int32_t rx = static_cast<int32_t>(star_prng_next(prng_state));
		star.offset_fp[0] = (rx - 0x8000) << 9;
		const int32_t ry = static_cast<int32_t>(star_prng_next(prng_state));
		star.offset_fp[1] = (ry - 0x8000) << 9;
		const int32_t abs_x = star.offset_fp[0] < 0 ? -star.offset_fp[0] : star.offset_fp[0];
		const int32_t abs_y = star.offset_fp[1] < 0 ? -star.offset_fp[1] : star.offset_fp[1];
		const int32_t rz = static_cast<int32_t>(star_prng_next(prng_state));
		star.offset_fp[2] = ((rz + 0x20000) << 6) - ((abs_x + abs_y) >> 3);
		const int32_t rb = static_cast<int32_t>(star_prng_next(prng_state));
		star.billboard_param = (rb & 0x3FF) + 12288;
		const int32_t rm = static_cast<int32_t>(star_prng_next(prng_state));
		star.twinkle_mask = 31 >> (rm & 3);
		const int32_t ra = static_cast<int32_t>(star_prng_next(prng_state)) & 0xFF;
		star.twinkle_add = ra == 0 ? 1 : ra;
		const int32_t max_add = 255 - star.twinkle_mask;
		if (star.twinkle_add > max_add) {
			star.twinkle_add = max_add;
		}
		// dir = normalize(off >> 8) via 2^32/len with +0x8000 rounding; a
		// zero integer length leaves the >>8 values unnormalized (the
		// original stores them first and guards the divide).
		int32_t scaled[3];
		double sum_sq = 0.0;
		for (int c = 0; c < 3; ++c) {
			scaled[c] = star.offset_fp[c] >> 8;
			star.dir_fp[c] = scaled[c];
			sum_sq += static_cast<double>(scaled[c]) * static_cast<double>(scaled[c]);
		}
		const int32_t len = star_length_int(std::sqrt(sum_sq));
		if (len != 0) {
			const int64_t inv = static_cast<int64_t>(0x100000000LL / len);
			for (int c = 0; c < 3; ++c) {
				star.dir_fp[c] = static_cast<int32_t>(
						static_cast<uint64_t>(inv * static_cast<int64_t>(scaled[c]) + 0x8000) >> 16);
			}
		}
	}
}

int32_t star_twinkle_tick(StarInstance &star, uint32_t &prng_state) {
	// [orig: render_star_field @ 0x5adb45]
	const int32_t r = static_cast<int32_t>(star_prng_next(prng_state));
	star.brightness = (star.brightness + star.twinkle_add + (r & star.twinkle_mask)) >> 1;
	return star.brightness;
}

bool star_visible_fixed(const StarInstance &star, const int32_t light_dir_fp[3]) {
	// [orig: render_star_field @ 0x5adac4] — hidden when the 16.16 dot
	// exceeds 64225 (~0.98).
	const int64_t dot = (static_cast<int64_t>(light_dir_fp[0]) * star.dir_fp[0] +
								static_cast<int64_t>(light_dir_fp[1]) * star.dir_fp[1] +
								static_cast<int64_t>(light_dir_fp[2]) * star.dir_fp[2]) >>
			16;
	return dot <= 64225;
}

} // namespace opennova::env
