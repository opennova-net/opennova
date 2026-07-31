// Engine-faithful atmosphere math, asserted against the exact fixed-point
// behavior witnessed in Jointops.exe. RE record: docs/env/env-tod-re.md.
#include <env/env_celestial.h>
#include <env/env_water_render.h>
#include <env/env_weather.h>

#include <cmath>
#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near(float actual, float expected, float epsilon = 0.001f) {
	return std::fabs(actual - expected) <= epsilon;
}

int byte_of(float normalized) {
	return static_cast<int>(normalized * 255.0f + 0.5f);
}

} // namespace

int main() {
	using namespace opennova::env;

	// --- Fog policy [orig: Render_SetFogState @ 0x58a950] -------------------
	{
		const FogParams exp_fog = compute_fog_params(0, 1000.0f, 0.0f);
		if (!expect(exp_fog.exponential, "fog type 0 is exponential")) return 1;
		if (!expect(near(exp_fog.exp_density, 4.1588831f / 1000.0f, 1e-7f), "type 0 density is ln(64)/end")) return 1;

		const FogParams near_start = compute_fog_params(1, 1000.0f, 0.0f);
		if (!expect(!near_start.exponential && near(near_start.start, 0.5f), "type 1 starts at 0.5 units")) return 1;

		const FogParams half = compute_fog_params(2, 1000.0f, 0.0f);
		if (!expect(near(half.start, 500.0f), "type 2 starts at half end")) return 1;

		const FogParams quarter = compute_fog_params(3, 1000.0f, 0.0f);
		if (!expect(near(quarter.start, 250.0f), "type 3 starts at quarter end")) return 1;

		const FogParams overcast_half = compute_fog_params(2, 1000.0f, 0.5f);
		if (!expect(near(overcast_half.start, 250.0f), "overcast scales the linear start by (1-density)")) return 1;

		const FogParams degenerate = compute_fog_params(1, 0.5f, 0.0f);
		if (!expect(!degenerate.enabled, "start == end disables fog")) return 1;

		if (!expect(near(fog_end_above_water(1000.0f, 1.0f), 500.0f), "full overcast halves the fog end")) return 1;
		if (!expect(near(fog_end_underwater(0.8f), 25.4f, 0.2f), "murk 0.8 gives ~25 units underwater")) return 1;
	}

	// --- Day phase windows [orig: Environment_ComputeTimeOfDayColors @ 0x57de99]
	{
		const DayPhase midnight = compute_day_phase(0.0f);
		if (!expect(midnight.is_night && near(midnight.blend, 1.0f), "midnight is full night")) return 1;

		const DayPhase pre_dawn = compute_day_phase(550.0f);
		if (!expect(pre_dawn.is_night && near(pre_dawn.blend, 0.5f), "05:50 fades night out (0.5)")) return 1;

		const DayPhase dawn_switch = compute_day_phase(600.0f);
		if (!expect(!dawn_switch.is_night && near(dawn_switch.blend, 0.0f), "06:00 switches to day at blend 0")) return 1;

		const DayPhase morning = compute_day_phase(610.0f);
		if (!expect(!morning.is_night && near(morning.blend, 0.5f), "06:10 fades day in (0.5)")) return 1;

		const DayPhase noon = compute_day_phase(1200.0f);
		if (!expect(!noon.is_night && near(noon.blend, 1.0f), "noon is full day")) return 1;

		const DayPhase pre_dusk = compute_day_phase(1835.0f);
		if (!expect(!pre_dusk.is_night && near(pre_dusk.blend, 0.5f), "18:35 fades day out (0.5)")) return 1;

		const DayPhase dusk_switch = compute_day_phase(1845.0f);
		if (!expect(dusk_switch.is_night && near(dusk_switch.blend, 0.0f), "18:45 switches to night at blend 0")) return 1;

		const DayPhase night = compute_day_phase(1905.0f);
		if (!expect(night.is_night && near(night.blend, 1.0f), "19:05 is full night")) return 1;
	}

	// --- Integer smoothing [orig: Environment_UpdateWeatherTick @ 0x57ee78] -
	{
		if (!expect(smooth_eighth(0, 80) == 10, "eighth step rounds (80+7)>>3 = 10")) return 1;
		if (!expect(smooth_eighth(79, 80) == 80, "eighth step lands exactly from below")) return 1;
		// The +7 bias means values within 7 ABOVE the target stall — engine quirk.
		if (!expect(smooth_eighth(5, 0) == 5, "eighth step stalls within 7 above the target")) return 1;
		if (!expect(smooth_eighth(20, 0) == 18, "eighth step from above: (0-20+7)>>3 = -2")) return 1;

		if (!expect(spring_step(0, 320, 5, 1000) == 5, "spring step clamps to +step_clamp")) return 1;
		if (!expect(spring_step(0, -320, 5, 1000) == -5, "spring step clamps to -step_clamp")) return 1;
		if (!expect(spring_step(998, 4000, 64, 1000) == 1000, "spring result clamps to max_abs")) return 1;
	}

	// --- Scalar spring channels (env #27 wiring) [orig: the weather tick's
	// scalar tail @ 0x57edd7..0x57ef92; targets-only snap @ 0x57d1e0] --------
	{
		EnvScalarChannels ch;
		if (!expect(ch.fog_dist_fp == (1024 << 16), "fog dist boots at the 1024 default")) return 1;
		if (!expect(ch.sky_height_fp == (175 << 16), "sky height boots at the 175 authoring default")) return 1;
		ch.fog_dist_target_fp = 500 << 16;
		ch.sky_height_target_fp = 200 << 16;
		ch.tick();
		// fog spring: ((500-1024)<<16 + 31) >> 5 = -1073152 (arithmetic shift).
		if (!expect(ch.fog_dist_fp == (1024 << 16) - 1073152, "fog dist takes the witnessed 1/32 spring step")) return 1;
		// sky eighth-snap: ((200-175)<<16 + 7) >> 3 = 204800.
		if (!expect(ch.sky_height_fp == (175 << 16) + 204800, "sky height takes the witnessed 1/8 step")) return 1;
		for (int i = 0; i < 2048; ++i) {
			ch.tick();
		}
		if (!expect(ch.fog_dist_fp >= (500 << 16) && ch.fog_dist_fp <= (500 << 16) + 31,
					"fog dist converges from above into the [target, target+31] parking window")) return 1;
		if (!expect(ch.sky_height_fp == (200 << 16), "sky height overshoot-snaps onto the target")) return 1;
		if (!expect(ch.rain_pct_fp == 0 && ch.sun_dim_fp == 0 && ch.overcast_fp == 0,
					"untargeted channels hold their defaults")) return 1;
	}

	// --- Star field (env #33) [orig: Star_GenerateInstanceTable @ 0x5ac850;
	// render_star_field @ 0x5ad9c0] ------------------------------------------
	{
		uint32_t prng = 1u;
		// The PRNG's first draws from state 1 (hand-derived from the witnessed
		// rol4(s + rol11(s)) ^ 1): 0x8011, 0x8111.
		uint32_t check_state = 1u;
		if (!expect(star_prng_next(check_state) == 0x8011u, "star PRNG first draw from seed 1")) return 1;
		if (!expect(star_prng_next(check_state) == 0x8111u, "star PRNG second draw")) return 1;

		static StarInstance stars[kStarInstanceCount];
		generate_star_instances(stars, prng);
		// star[0] from seed 1: offX = (0x8011-0x8000)<<9, offY = (0x8111-0x8000)<<9.
		if (!expect(stars[0].offset_fp[0] == (0x11 << 9), "star0 offX")) return 1;
		if (!expect(stars[0].offset_fp[1] == (0x111 << 9), "star0 offY")) return 1;
		bool invariants = true;
		for (int i = 0; i < kStarInstanceCount; ++i) {
			const StarInstance &st = stars[i];
			if (st.billboard_param < 12288 || st.billboard_param > 12288 + 0x3FF) invariants = false;
			if (st.twinkle_mask != 31 && st.twinkle_mask != 15 && st.twinkle_mask != 7 && st.twinkle_mask != 3) invariants = false;
			if (st.twinkle_add < 1 || st.twinkle_add > 255 - st.twinkle_mask) invariants = false;
			if (st.brightness != 0) invariants = false;
			const long long len2 = 1LL * st.dir_fp[0] * st.dir_fp[0] +
					1LL * st.dir_fp[1] * st.dir_fp[1] + 1LL * st.dir_fp[2] * st.dir_fp[2];
			// |dir| within ~1% of 1.0 in 16.16 (integer divide + rounding slack).
			const long long unit2 = 1LL << 32;
			if (len2 < unit2 * 98 / 100 || len2 > unit2 * 102 / 100) invariants = false;
		}
		if (!expect(invariants, "all 256 stars satisfy the witnessed field invariants")) return 1;

		// Twinkle: brightness EMA-chases add + (r & mask); bounded by 255.
		StarInstance tw = stars[0];
		uint32_t tw_prng = 99u;
		int32_t last = 0;
		for (int i = 0; i < 64; ++i) {
			last = star_twinkle_tick(tw, tw_prng);
			if (last < 0 || last > 255) { invariants = false; break; }
		}
		if (!expect(invariants && last >= tw.twinkle_add / 2, "twinkle accumulator stays bounded and lit")) return 1;

		// The near-light cull: a star straight at the light hides; opposite shows.
		StarInstance aligned;
		aligned.dir_fp[0] = 0; aligned.dir_fp[1] = 0; aligned.dir_fp[2] = 65536;
		const int32_t light_up[3] = { 0, 0, 65536 };
		const int32_t light_down[3] = { 0, 0, -65536 };
		if (!expect(!star_visible_fixed(aligned, light_up), "star inside the 0.98 cone hides")) return 1;
		if (!expect(star_visible_fixed(aligned, light_down), "star opposite the light shows")) return 1;
	}

	// --- Channel smoothing [orig: interpolate_weather_color @ 0x57d9e0] -----
	{
		ColorChannelState state;
		state.snap_to(0x00FF8040u);
		const uint32_t stepped = state.step(0x00000000u, 0x7FFFFFFF);
		// One unclamped step leaves 7/8 of each channel (with +0x80000 rounding).
		if (!expect(((stepped >> 16) & 0xFF) == 223, "R 255 decays to 223 after one eighth step")) return 1;
		if (!expect(((stepped >> 8) & 0xFF) == 112, "G 128 decays to 112")) return 1;
		if (!expect((stepped & 0xFF) == 56, "B 64 decays to 56")) return 1;

		ColorChannelState clamped;
		clamped.snap_to(0x00FF0000u);
		const uint32_t limited = clamped.step(0x00000000u, 1 << 20);
		if (!expect(((limited >> 16) & 0xFF) == 254, "per-channel max step limits decay to 1/255 per tick")) return 1;
	}

	// --- Lightning [orig: Environment_SetLightningFlash @ 0x57d320] ---------
	{
		if (!expect(lightning_flash_level(kLightningSequenceA, 5, 10) == 200, "sequence A tick 10 flashes 200")) return 1;
		if (!expect(lightning_flash_level(kLightningSequenceA, 5, 6) == 255, "sequence A tick 6 flashes 255")) return 1;
		if (!expect(lightning_flash_level(kLightningSequenceA, 5, 5) == -1, "sequence A tick 5 is not an epoch")) return 1;
		if (!expect(lightning_flash_level(kLightningSequenceB, 7, 23) == 100, "sequence B tick 23 flashes 100")) return 1;

		const LightningAdditives add = lightning_additives({1.0f, 1.0f, 1.0f}, 255);
		if (!expect(byte_of(add.sky.r) == 254, "sky additive is (255*255)>>8")) return 1;
		if (!expect(byte_of(add.fog.r) == 127, "fog additive is (255*255)>>9")) return 1;
		if (!expect(byte_of(add.ground.r) == 63, "ground additive is (255*255)>>10")) return 1;
	}

	// --- Sun glare [orig: compute_sun_glare_and_fog_blend @ 0x5ad610] -------
	{
		const GlareResult full = compute_sun_glare(1.0f, 255);
		if (!expect(full.glare == 192 && full.fog_whiten == 40, "dot 1.0 gives glare 192 / whiten 40")) return 1;

		const GlareResult partial = compute_sun_glare(0.95f, 255);
		if (!expect(partial.glare == 37, "dot 0.95: 0.95^32 * 192 = 37")) return 1;
		if (!expect(partial.fog_whiten == 0, "dot 0.95: 0.95^128 * 40 truncates to 0")) return 1;

		const GlareResult occluded = compute_sun_glare(1.0f, 128);
		if (!expect(occluded.glare == 96, "occlusion brightness scales glare (192*128/255)")) return 1;

		if (!expect(compute_sun_glare(-0.5f, 255).glare == 0, "looking away yields no glare")) return 1;

		if (!expect(glare_brightness_step(0, 256) == 16, "brightness rises 16/frame")) return 1;
		if (!expect(glare_brightness_step(248, 256) == 248, "dead-band holds within +-16 (no snap)")) return 1;
		if (!expect(glare_brightness_step(100, 0) == 84, "brightness falls 16/frame")) return 1;
		if (!expect(glare_brightness_step(272, 256) == 256, "steps down onto the band edge")) return 1;
	}

	// --- Derived render colors [orig: Environment_UpdateWeatherTick tail] ---
	{
		const Rgb combined = combine_terrain_light({1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f});
		if (!expect(byte_of(combined.r) == 180, "terrain light weight is 0xB5/256")) return 1;

		const Rgb low = combine_terrain_light_low({1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f});
		if (!expect(byte_of(low.r) == 89, "secondary light weight is 0x5A/256")) return 1;

		const Rgb lit = lit_water_color({128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f},
		                                {128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f});
		if (!expect(byte_of(lit.r) == 128, "water*light>>7 is identity at mid-gray")) return 1;

		const Rgb doubled = double_saturate({200.0f / 255.0f, 100.0f / 255.0f, 0.0f});
		if (!expect(byte_of(doubled.r) == 255 && byte_of(doubled.g) == 200,
		            "fog render colors double with saturation")) return 1;
	}

	// --- BMS overrides [orig: Game_LoadTerrainDuringConnect @ 0x520710] -----
	{
		Config config;
		config.keyframes.resize(2);
		config.keyframes[0].time = 0;
		config.keyframes[0].fog = {10.0f / 255.0f, 10.0f / 255.0f, 10.0f / 255.0f};
		config.keyframes[1].time = 1200;
		config.keyframes[1].fog = {20.0f / 255.0f, 20.0f / 255.0f, 20.0f / 255.0f};

		BmsEnvOverrides overrides;
		overrides.has_water_height = true;
		overrides.water_height = 5.0f;
		overrides.has_fog_level = true;
		overrides.fog_level = 333.0f;
		overrides.has_fog_color = true;
		overrides.fog_color = {50.0f / 255.0f, 60.0f / 255.0f, 70.0f / 255.0f};
		overrides.has_water_murk = true;
		overrides.water_murk = 0.5f;
		overrides.has_start_time = true;
		overrides.start_time = 330;

		apply_bms_overrides(config, overrides);
		if (!expect(config.water_height_set && near(config.water_height, 5.0f), "water height override applies")) return 1;
		if (!expect(near(config.fog_level, 333.0f), "fog level override applies")) return 1;
		if (!expect(near(config.keyframes[0].fog.r, 50.0f / 255.0f) && near(config.keyframes[1].fog.r, 50.0f / 255.0f),
		            "fog color override replaces every keyframe's fog")) return 1;
		if (!expect(near(config.water_murk, 0.5f), "murk override applies")) return 1;
		if (!expect(config.curtime == 330, "start time override applies")) return 1;

		Config untouched;
		const float default_fog = untouched.fog_level;
		apply_bms_overrides(untouched, BmsEnvOverrides{});
		if (!expect(near(untouched.fog_level, default_fog) && !untouched.water_height_set,
		            "empty override set leaves the config alone")) return 1;
	}

	// Horizon blend — the frame-clear cross-fade, byte-exact vs the witnessed
	// MMX sequence [orig: Environment_UpdateWeatherTick @ 0x57f037..0x57f0a1].
	{
		const Rgb fog{1.0f, 0.0f, 128.0f / 255.0f};
		const Rgb sky{0.0f, 1.0f, 64.0f / 255.0f};
		const uint32_t ref = kFogDistReferenceDefault; // 1024.0 in 16.16

		// At or above ref/2 the skyfog is untouched.
		Rgb out = horizon_blend_skyfog(fog, sky, 512u << 16, ref);
		if (!expect(byte_of(out.r) == 0 && byte_of(out.g) == 255 && byte_of(out.b) == 64,
		            "horizon blend leaves skyfog untouched at ref/2")) return 1;

		// At or below ref/4: pure fog color through the byte pipeline —
		// 255 -> 255 and 128 -> 128 exactly; the witnessed low-byte loss maps
		// a 1/255 channel to 0 (replicated, not "fixed").
		out = horizon_blend_skyfog(fog, sky, 256u << 16, ref);
		if (!expect(byte_of(out.r) == 255 && byte_of(out.g) == 0 && byte_of(out.b) == 128,
		            "horizon blend is pure fog at ref/4")) return 1;
		out = horizon_blend_skyfog(Rgb{1.0f / 255.0f, 0.0f, 0.0f}, sky, 100u << 16, ref);
		if (!expect(byte_of(out.r) == 0, "the witnessed low-byte loss at t=0 (1 -> 0)")) return 1;

		// Midpoint of the band (dist = 3*ref/8 -> t = 0x8000): both pmulhw legs
		// land on 8191 >> 6 = 127 for a 255 input.
		out = horizon_blend_skyfog(fog, sky, 384u << 16, ref);
		if (!expect(byte_of(out.r) == 127 && byte_of(out.g) == 127,
		            "mid-band blends 255/0 and 0/255 to 127")) return 1;

		// Three-quarters through the band (dist = 448 -> t = 0xC000): the fog
		// leg lands (32767*8191)>>16 = 4095 >> 6 = 63, the skyfog leg
		// (32767*24576)>>16 = 12287 >> 6 = 191.
		out = horizon_blend_skyfog(fog, sky, 448u << 16, ref);
		if (!expect(byte_of(out.r) == 63 && byte_of(out.g) == 191,
		            "t=0xC000 blends 255-legs to 63/191")) return 1;

		// The band's top edge (one fixed-point step below ref/2, t = 0xFFFF)
		// reproduces skyfog exactly through the byte pipeline.
		out = horizon_blend_skyfog(fog, sky, (512u << 16) - 1, ref);
		if (!expect(byte_of(out.r) == 0 && byte_of(out.g) == 255,
		            "the band's top edge reproduces skyfog")) return 1;
	}

	// Terrain tint — the FULL/HALF split and the two live consumers
	// [orig: PolyTrn_SetTerrainTintColors @ 0x605e20].
	{
		// Retail default 255,255,255.
		TerrainTint tint = terrain_tint_from_packed(0x00FFFFFFu);
		if (!expect(tint.full == 0xFFFFFFFFu && tint.half == 0xFF7F7F7Fu,
		            "default tint splits to FULL FFFFFFFF / HALF FF7F7F7F")) return 1;

		// A mid color: 0x8040C0 -> half = 0x40 0x20 0x60.
		tint = terrain_tint_from_packed(0x008040C0u);
		if (!expect(tint.full == 0xFF8040C0u && tint.half == 0xFF402060u,
		            "mid tint halves per channel with the 0x7F mask")) return 1;

		// The parsed-Rgb path packs bytes/255 floats back to the same split.
		tint = terrain_tint_from_rgb(Rgb{128.0f / 255.0f, 64.0f / 255.0f, 192.0f / 255.0f});
		if (!expect(tint.full == 0xFF8040C0u && tint.half == 0xFF402060u,
		            "Rgb path packs to the same tint split")) return 1;

		// Foliage lightmap tint [orig: sample_terrain_colormap_tinted @ 0x606030]:
		// 128 is identity, 255 saturates ~2x, alpha passes through.
		const uint32_t full_identity = 0xFF808080u;
		uint32_t out = foliage_lightmap_tint(0x40C08020u, full_identity);
		if (!expect(out == 0x40C08020u, "foliage tint 128 is identity")) return 1;
		out = foliage_lightmap_tint(0x20FF8001u, 0xFFFFFFFFu);
		// 255*255>>7 = 508 -> 255; 128*255>>7 = 255; 1*255>>7 = 1.
		if (!expect(out == 0x20FFFF01u, "foliage tint 255 saturates, alpha passes")) return 1;
		out = foliage_lightmap_tint(0xFF804020u, 0xFF402060u);
		// 128*64>>7 = 64; 64*32>>7 = 16; 32*96>>7 = 24.
		if (!expect(out == 0xFF401018u, "foliage tint modulates per channel >> 7")) return 1;

		// Tile overlay factor: MODULATE2X over HALF -> 254/255 at the default
		// (the witnessed one-LSB-dark near-identity).
		const Rgb factor = tile_overlay_tint_factor(terrain_tint_from_packed(0x00FFFFFFu));
		if (!expect(byte_of(factor.r) == 254 && byte_of(factor.g) == 254 && byte_of(factor.b) == 254,
		            "default tile-overlay factor is 254/255, not exact identity")) return 1;
		const Rgb mid = tile_overlay_tint_factor(terrain_tint_from_packed(0x008040C0u));
		if (!expect(byte_of(mid.r) == 128 && byte_of(mid.g) == 64 && byte_of(mid.b) == 192,
		            "even-channel tile-overlay factor recovers the packed color")) return 1;
	}

	// Iris auto-exposure — the witnessed curve [orig: terrain_sector_compute_lighting
	// @ 0x5c7550]. lum = 0.25*(r+b) + 0.5*g; gain = 0.01*(pct*base/(2m) + (100-pct)*base).
	{
		// Luminance is exact.
		if (!expect(near(iris_luminance(Rgb{1.0f, 0.0f, 1.0f}), 0.5f), "iris lum of (1,0,1) = 0.5"))
			return 1;
		if (!expect(near(iris_luminance(Rgb{0.0f, 1.0f, 0.0f}), 0.5f), "iris lum of (0,1,0) = 0.5"))
			return 1;
		if (!expect(near(iris_luminance(Rgb{1.0f, 1.0f, 1.0f}), 1.0f), "iris lum of white = 1.0")) return 1;

		// Defaults iris_center 1.25 / iris_percent 50: base = 80, gain = 40 + 20/m.
		// A pure white sky (dir/ground 0) gives m = 1 -> gain = 60 (the record's
		// "~60 in bright sun"). int-truncated; allow ±1 for float truncation.
		{
			const int g = iris_gain(Rgb{0, 0, 0}, Rgb{1, 1, 1}, Rgb{0, 0, 0}, 0, 1, 0, 1.25f, 50.0f);
			if (!expect(g >= 59 && g <= 60, "iris gain at m=1 defaults is 60")) return 1;
		}
		// Darkness (all blocks zero) drives the base/(2m) term to the 255 clamp.
		if (!expect(iris_gain(Rgb{0, 0, 0}, Rgb{0, 0, 0}, Rgb{0, 0, 0}, 0, 1, 0, 1.25f, 50.0f) == 255,
		            "iris gain clamps to 255 in full darkness")) return 1;
		// A dimmer scene (m = 0.5, sky {0.5,0.5,0.5} -> lum 0.5) -> 40 + 20/0.5 = 80.
		{
			const int g = iris_gain(Rgb{0, 0, 0}, Rgb{0.5f, 0.5f, 0.5f}, Rgb{0, 0, 0}, 0, 1, 0, 1.25f, 50.0f);
			if (!expect(g >= 79 && g <= 80, "iris gain at m=0.5 defaults is 80")) return 1;
		}
		// iris_percent 0 removes the exposure-boost term: gain = 0.01*100*base = base.
		// base = 1.25*64 = 80 -> gain 80, independent of m.
		{
			const int g = iris_gain(Rgb{0, 0, 0}, Rgb{1, 1, 1}, Rgb{0, 0, 0}, 0, 1, 0, 1.25f, 0.0f);
			if (!expect(g >= 79 && g <= 80, "iris_percent 0 gives base = 80")) return 1;
		}
	}

	// --- Weather oscillator [orig: Environment_UpdateWeatherTick @ 0x57e9b0] --
	{
		// The witnessed PRNG sequence from the mission-start seed
		// [orig: seed 0x12333333 — the mov imm32 @ 0x57d2ff in
		//  Environment_SnapStateToTargets @ 0x57d1e0 (0x12345633 was a reimpl
		//  transcription error, env #25; the WAC RNG @ 0x4f966b shares the
		//  constant); step rol9 + ((int32)x >> 31) & 0x1ABB09 @ 0x57e9fc..0x57ea16].
		const uint32_t expected_words[8] = {
			0x66666624u, 0xCCE703D5u, 0xCE2266A2u, 0x44CD459Cu,
			0x9AA5F392u, 0x4BE72535u, 0xCE6525A0u, 0xCA65FCA5u,
		};
		WeatherOscillator prng_probe;
		for (int i = 0; i < 8; ++i) {
			const uint32_t word = prng_probe.reroll();
			if (word != expected_words[i]) {
				std::fprintf(stderr, "FAIL: PRNG word %d = %08X, want %08X\n", i, word, expected_words[i]);
				return 1;
			}
		}
		// The signed-carry idiom is load-bearing: a logical-shift port (adds
		// 0/1 instead of 0/0x1ABB09) forks at the first negative rotate —
		// word 1 becomes 0xCCCC48CD. Guard the divergence explicitly.
		if (!expect(expected_words[1] != 0xCCCC48CDu && expected_words[1] == 0xCCE703D5u,
		            "PRNG carry is the signed 0x1ABB09 idiom, not bit-31")) return 1;

		// Still air (intensity 0): the spring settle toward 0x8000 is
		// PRNG-independent and matches the committed GUT wa/* vectors.
		WeatherOscillator still;
		still.intensity = 0;
		const struct { int tick; int smoothed; int index; } still_landmarks[] = {
			{1, 0x0000, 0x01}, {4, 0x0250, 0x04}, {16, 0x3F01, 0x10},
			{64, 0x726E, 0x40}, {256, 0x7DDF, 0x00},
		};
		int ticks_done = 0;
		for (const auto &lm : still_landmarks) {
			for (; ticks_done < lm.tick; ++ticks_done) {
				still.tick();
			}
			if (still.smoothed != lm.smoothed || still.ring_index != lm.index) {
				std::fprintf(stderr, "FAIL: still-air k%03d = %04X/%02X, want %04X/%02X\n",
				             lm.tick, still.smoothed, still.ring_index, lm.smoothed, lm.index);
				return 1;
			}
		}

		// Wind at the WITNESSED intensity (Env_WindScale = 256, its only
		// retail value [orig: Environment_InitDefaults @ 0x57c1d1; sole other
		// xref is the tick read]): a stable ambient sway around the 0x8000
		// rest. The noise feedback term 15*prev has gain 15*intensity/4096 —
		// stable only for intensity <= 273. The old GDScript node scaled
		// wind_strength onto 0..8192, driving the 32-bit state divergent and
		// "surviving" via 64-bit wrap + clamps; that mapping was unwitnessed
		// (divergence noted in docs/env/env-tod-re.md).
		WeatherOscillator wind;
		wind.intensity = 256;
		const struct { int tick; int smoothed; } wind_landmarks[] = {
			{1, 0x0012}, {16, 0x56E5}, {64, 0x7CDE}, {256, 0x9787},
		};
		ticks_done = 0;
		for (const auto &lm : wind_landmarks) {
			for (; ticks_done < lm.tick; ++ticks_done) {
				wind.tick();
			}
			if (wind.smoothed != lm.smoothed) {
				std::fprintf(stderr, "FAIL: wind k%03d = %04X, want %04X\n",
				             lm.tick, wind.smoothed, lm.smoothed);
				return 1;
			}
		}
		if (!expect(wind.amp_ring[wind.ring_index] >= 0, "amp ring floors at 0")) return 1;
	}

	// --- Lightning sequencers [orig: @ 0x57ec6f (A) / @ 0x57ed0a (B)] --------
	{
		// Short (timer A = 16): epoch table {10:C8, 6:FF, 4:C8, 2:FF, 0:0};
		// per-tick levels match the committed GUT wc/short_seq bytes.
		LightningSequencers seq_short;
		seq_short.trigger_short();
		const int expected_short[16] = {
			0, 0, 0, 0, 0, 200, 200, 200, 200, 255, 255, 200, 200, 255, 255, 0,
		};
		for (int i = 0; i < 16; ++i) {
			seq_short.tick();
			if (seq_short.level != expected_short[i]) {
				std::fprintf(stderr, "FAIL: short seq tick %d = %d, want %d\n",
				             i + 1, seq_short.level, expected_short[i]);
				return 1;
			}
		}

		// Long (timer B = 32): SET semantics per epoch — the witnessed
		// staircase C8 C8 C8 96 96 C8 C8 96 64 32 32 00 (the GDScript port's
		// max-combining plateau C8 x11 was unwitnessed embellishment;
		// Environment_SetLightningFlash overwrites @ 0x57d320).
		LightningSequencers seq_long;
		seq_long.trigger_long();
		const int expected_long[12] = {
			200, 200, 200, 150, 150, 200, 200, 150, 100, 50, 50, 0,
		};
		for (int i = 0; i < 12; ++i) {
			seq_long.tick();
			if (seq_long.level != expected_long[i]) {
				std::fprintf(stderr, "FAIL: long seq tick %d = %d, want %d\n",
				             i + 1, seq_long.level, expected_long[i]);
				return 1;
			}
		}

		// Packed additives at lightning 0x383B27, level 200
		// [orig: Environment_SetLightningFlash @ 0x57d320]: truncating
		// per-byte (c*level) >> {8,9,10}.
		const LightningAdditivesPacked add = lightning_additives_packed(0x383B27u, 200);
		if (!expect(add.sky == 0x2B2E1Eu, "packed sky additive >> 8")) return 1;
		if (!expect(add.fog == 0x15170Fu && add.skyfog == add.fog, "packed fog/skyfog additive >> 9")) return 1;
		if (!expect(add.ground == 0x0A0B07u, "packed ground additive >> 10")) return 1;
	}

	// --- Rain factor + weather color block [orig: interpolate_weather_color
	//     @ 0x57d9e0] --------------------------------------------------------
	{
		if (!expect(rain_blend_factor(0) == 0x8000, "rain factor at 0")) return 1;
		if (!expect(rain_blend_factor(0x4000) == 0x4000, "rain factor at half")) return 1;
		if (!expect(rain_blend_factor(0x8001) == 0, "rain factor over-range zeroes")) return 1;

		// With identity modulator (byte 64), zero rain, zero additive, the
		// block pipeline reduces EXACTLY to the ColorChannelState step.
		WeatherColorBlock block;
		block.snap(0x000000u);
		ColorChannelState plain;
		plain.snap_to(0x000000u);
		block.target = 0x00FFC080u;
		for (int i = 0; i < 8; ++i) {
			block.tick(kModulatorIdentityPacked, 0);
			const uint32_t expected = plain.step(0x00FFC080u, 255 << 20);
			if (block.render_color != expected || block.pre_mod_color != expected) {
				std::fprintf(stderr, "FAIL: block/step equivalence tick %d: %08X vs %08X\n",
				             i, block.render_color, expected);
				return 1;
			}
		}

		// Additive saturates per byte (paddusb): stepped 0x545859 + fog slot
		// 0x15170F = 0x696F68; near-white + additive pins the 0xFF ceiling.
		WeatherColorBlock add_block;
		add_block.snap(0x545859u);
		add_block.target = 0x545859u;
		add_block.additive = 0x15170Fu;
		add_block.tick(kModulatorIdentityPacked, 0);
		if (!expect(add_block.pre_mod_color == 0x696F68u, "block additive paddusb")) return 1;
		add_block.snap(0x00FFF0F8u);
		add_block.target = 0x00FFF0F8u;
		add_block.additive = 0x00202020u;
		add_block.tick(kModulatorIdentityPacked, 0);
		if (!expect(add_block.pre_mod_color == 0x00FFFFFFu, "block additive saturates")) return 1;

		// Rain at 0x4000 halves every channel (truncating): the witnessed
		// ((c*m)>>1 * (factor>>4)) >> 16 chain on 0x80FF40C8 -> 0x407F2064.
		WeatherColorBlock rain_block;
		rain_block.snap(0x80FF40C8u);
		rain_block.target = 0x80FF40C8u;
		rain_block.tick(kModulatorIdentityPacked, 0x4000);
		if (!expect(rain_block.render_color == 0x407F2064u, "block rain-half modulation")) return 1;
		if (!expect(rain_block.pre_mod_color == 0x80FF40C8u, "pre-mod color unaffected by rain")) return 1;

		// The world-driven sky cluster keeps skyfog, the static colors, and the six
		// dome ramps in witnessed order while delegating every channel to the
		// same WeatherColorBlock pipeline.
		SkyWeatherColorBlocks sky_blocks;
		sky_blocks.snap({0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u});
		sky_blocks.set_targets({
				0x00081018u,
				0x00102030u,
				0x00182838u,
				0x00204060u,
				0x00284868u,
				0x00305070u,
				0x00385878u,
				0x00406080u,
				0x00486888u,
				0x00507090u,
		});
		sky_blocks.set_skyfog_additive(0x00010101u);
		sky_blocks.tick_skyfog(kModulatorIdentityPacked, 0);
		sky_blocks.tick_statics(kModulatorIdentityPacked, 0);
		sky_blocks.tick_dome(kModulatorIdentityPacked, 0);
		if (!expect(sky_blocks.skyfog.render_color == 0x00020304u,
		            "skyfog uses the block pipeline plus its lightning additive")) return 1;
		if (!expect(sky_blocks.ceiling.render_color == 0x00020406u &&
		                    sky_blocks.cloud.render_color == 0x00030507u &&
		                    sky_blocks.floor.render_color == 0x0004080Cu &&
		                    sky_blocks.ceiling.pre_mod_color == 0x00020406u &&
		                    sky_blocks.floor.pre_mod_color == 0x0004080Cu,
		            "all three static colors take one pre- and post-mod eighth-step")) return 1;
		if (!expect(sky_blocks.skybase.render_color == 0x0005090Du &&
		                    sky_blocks.skybright.render_color == 0x00060A0Eu &&
		                    sky_blocks.skyhighlight.render_color == 0x00070B0Fu,
		            "all three sky ramps take one 12.20 eighth-step")) return 1;
		if (!expect(sky_blocks.cloudbase.render_color == 0x00080C10u &&
		                    sky_blocks.cloudhighlight.render_color == 0x00090D11u &&
		                    sky_blocks.cloudedge.render_color == 0x000A0E12u,
		            "all three cloud ramps take the same weather-block path")) return 1;
	}

	// --- Cloud scroll [orig: rate ramp @ 0x57eecc; accumulators
	//     @ 0x57f1a5..0x57f1d1] ----------------------------------------------
	{
		// Rate ramps by smooth_eighth toward sky_speed << 10 (15 << 10 =
		// 15360); the accumulators advance {1, 1, 2/3, 4/3} with truncating /3.
		CloudScrollState scroll;
		const struct { int tick; int rate; int32_t l1; int32_t l2v; int32_t l2u; } landmarks[] = {
			{1, 1920, 1920, 1280, 2560},
			{2, 3600, 5520, 3680, 7360},
			{3, 5070, 10590, 7060, 14120},
			{8, 10084, 52312, 34876, 69748},
			{64, 15360, 875722, 583835, 1167609},
		};
		int ticks_done = 0;
		for (const auto &lm : landmarks) {
			for (; ticks_done < lm.tick; ++ticks_done) {
				scroll.tick(15 << 10);
			}
			if (scroll.rate != lm.rate || scroll.acc_l1_u != lm.l1 || scroll.acc_l1_v != lm.l1 ||
			    scroll.acc_l2_v != lm.l2v || scroll.acc_l2_u != lm.l2u) {
				std::fprintf(stderr, "FAIL: scroll tick %d = rate %d accs %d/%d/%d/%d\n",
				             lm.tick, scroll.rate, scroll.acc_l1_u, scroll.acc_l1_v,
				             scroll.acc_l2_v, scroll.acc_l2_u);
				return 1;
			}
		}
		// The truncating /3 (rate 1025: 1025/3 = 341, not 342).
		CloudScrollState trunc;
		trunc.rate = 1025;
		trunc.tick(1025);
		if (!expect(trunc.acc_l2_v == 1025 - 341 && trunc.acc_l2_u == 1025 + 341,
		            "accumulator /3 truncates toward zero")) return 1;
	}

	// --- Sky dome mesh [orig: build_sky_dome_mesh @ 0x578db0] ----------------
	{
		// Reference-height build (v14 ~= 1): the dome the reimpl renders, with
		// the Y scale folded into the shader (env #20's ratified structure).
		const SkyDomeMesh ref = build_sky_dome_mesh(static_cast<float>(kSkyDomeReferenceHeight));
		if (!expect(static_cast<int>(ref.positions.size()) == kSkyDomeVertices * 3, "441 dome vertices")) return 1;
		if (!expect(static_cast<int>(ref.normals.size()) == kSkyDomeVertices * 3, "441 dome normals")) return 1;
		if (!expect(static_cast<int>(ref.uv1.size()) == kSkyDomeVertices * 2 &&
		            static_cast<int>(ref.uv2.size()) == kSkyDomeVertices * 2, "441 dome uv pairs x2")) return 1;
		if (!expect(static_cast<int>(ref.indices.size()) == kSkyDomeTriangles * 3, "2400 dome indices")) return 1;

		// Witnessed winding [orig: @ 0x578e00..0x578e86]: quads emit
		// (i, i+22, i+21), (i, i+1, i+22) — matches the committed sky/mesh
		// GUT vector "0 22 21 0 1 22 1 23 22 1 2 23".
		const int32_t head[12] = {0, 22, 21, 0, 1, 22, 1, 23, 22, 1, 2, 23};
		for (int i = 0; i < 12; ++i) {
			if (!expect(ref.indices[i] == head[i], "witnessed index winding (head)")) return 1;
		}
		// Last quad (row 19, col 19, base 418).
		const int32_t tail[6] = {418, 440, 439, 418, 419, 440};
		for (int i = 0; i < 6; ++i) {
			if (!expect(ref.indices[2394 + i] == tail[i], "witnessed index winding (tail)")) return 1;
		}

		// Apex v0: exact zeros, y == the input height within rounding
		// (v14 * y_unscaled(0) algebraically returns the height), normal
		// exactly +Y after normalize(0, y, 0).
		if (!expect(ref.positions[0] == 0.0f && ref.positions[2] == 0.0f, "apex x/z exactly 0")) return 1;
		if (!expect(near(ref.positions[1], 175.6906281f, 1e-4f), "dome pin: ref.positions[1], 175.6906281f, 1e-4f")) return 1;
		if (!expect(ref.normals[0] == 0.0f && ref.normals[1] == 1.0f && ref.normals[2] == 0.0f,
		            "apex normal is exactly +Y")) return 1;
		if (!expect(ref.uv1[0] == 0.0f && ref.uv2[1] == 0.0f, "apex uvs are 0")) return 1;

		// v22 (row 1, col 1): radius 51.2, theta pi/10.
		if (!expect(near(ref.positions[22 * 3 + 0], 15.8216705f, 1e-3f), "dome pin: ref.positions[22 * 3 + 0], 15.8216705f, 1e-3f")) return 1;
		if (!expect(near(ref.positions[22 * 3 + 1], 175.2639313f, 1e-4f), "dome pin: ref.positions[22 * 3 + 1], 175.2639313f, 1e-4f")) return 1;
		if (!expect(near(ref.positions[22 * 3 + 2], 48.6940956f, 1e-3f), "dome pin: ref.positions[22 * 3 + 2], 48.6940956f, 1e-3f")) return 1;
		if (!expect(near(ref.uv1[22 * 2 + 0], 0.0494427f, 1e-5f), "dome pin: ref.uv1[22 * 2 + 0], 0.0494427f, 1e-5f")) return 1;
		if (!expect(near(ref.uv1[22 * 2 + 1], 0.1521690f, 1e-5f), "dome pin: ref.uv1[22 * 2 + 1], 0.1521690f, 1e-5f")) return 1;
		if (!expect(near(ref.uv2[22 * 2 + 0], 0.0231763f, 1e-5f), "dome pin: ref.uv2[22 * 2 + 0], 0.0231763f, 1e-5f")) return 1;
		if (!expect(near(ref.uv2[22 * 2 + 1], 0.0713292f, 1e-5f), "dome pin: ref.uv2[22 * 2 + 1], 0.0713292f, 1e-5f")) return 1;
		// The anisotropic dome normal [orig: @ 0x578fbb], NOT the vertex dir.
		if (!expect(near(ref.normals[22 * 3 + 0], 0.0866516f, 1e-5f), "dome pin: ref.normals[22 * 3 + 0], 0.0866516f, 1e-5f")) return 1;
		if (!expect(near(ref.normals[22 * 3 + 1], 0.9598801f, 1e-5f), "dome pin: ref.normals[22 * 3 + 1], 0.9598801f, 1e-5f")) return 1;
		if (!expect(near(ref.normals[22 * 3 + 2], 0.2666864f, 1e-5f), "dome pin: ref.normals[22 * 3 + 2], 0.2666864f, 1e-5f")) return 1;

		// v220 (row 10, col 10): theta ~= pi — x collapses to ~0 (the float32
		// pi/10 seed keeps it sub-1e-3), z = -512.
		if (!expect(std::fabs(ref.positions[220 * 3 + 0]) < 1e-3f, "v220 x ~ 0 at theta ~ pi")) return 1;
		if (!expect(near(ref.positions[220 * 3 + 1], 132.7234802f, 1e-4f), "dome pin: ref.positions[220 * 3 + 1], 132.7234802f, 1e-4f")) return 1;
		if (!expect(near(ref.positions[220 * 3 + 2], -512.0f, 1e-3f), "dome pin: ref.positions[220 * 3 + 2], -512.0f, 1e-3f")) return 1;

		// Rim v440 (row 20, col 20): radius 1024 sits at y ~= 0 (exactly 0 in
		// pure reals; the float32 51.2 seed leaves ~-5e-6), z back at +1024,
		// uv2.z = 1024 * 3/2048 = 1.5.
		if (!expect(std::fabs(ref.positions[440 * 3 + 1]) < 1e-4f, "rim y ~ 0")) return 1;
		if (!expect(near(ref.positions[440 * 3 + 2], 1024.0f, 1e-3f), "dome pin: ref.positions[440 * 3 + 2], 1024.0f, 1e-3f")) return 1;
		if (!expect(near(ref.uv2[440 * 2 + 1], 1.5f, 1e-5f), "dome pin: ref.uv2[440 * 2 + 1], 1.5f, 1e-5f")) return 1;

		// Height scale is Y-ONLY [orig: @ 0x578ed4]: x/z bitwise identical
		// across heights, y scales by v14, normals tilt via y_scaled/v14^2.
		const SkyDomeMesh tall = build_sky_dome_mesh(250.0f);
		if (!expect(tall.positions[22 * 3 + 0] == ref.positions[22 * 3 + 0] &&
		            tall.positions[22 * 3 + 2] == ref.positions[22 * 3 + 2],
		            "height scale leaves x/z bitwise unchanged")) return 1;
		if (!expect(near(tall.positions[22 * 3 + 1], 249.3928375f, 1e-3f), "dome pin: tall.positions[22 * 3 + 1], 249.3928375f, 1e-3f")) return 1;
		if (!expect(near(tall.normals[22 * 3 + 0], 0.1186150f, 1e-5f), "dome pin: tall.normals[22 * 3 + 0], 0.1186150f, 1e-5f")) return 1;
		if (!expect(near(tall.normals[22 * 3 + 1], 0.9233970f, 1e-5f), "dome pin: tall.normals[22 * 3 + 1], 0.9233970f, 1e-5f")) return 1;
		if (!expect(near(tall.normals[22 * 3 + 2], 0.3650595f, 1e-5f), "dome pin: tall.normals[22 * 3 + 2], 0.3650595f, 1e-5f")) return 1;
		if (!expect(tall.indices == ref.indices, "indices are height-independent")) return 1;
	}

	// --- Water noise textures + UV state [orig: Water_GenerateNoiseTextures
	// @ 0x5c0360; Water_InitNoiseFieldAndSineLut @ 0x5c01a0;
	// render_water_surface @ 0x5c32c0] ------------------------------------
	{
		// PRNG_Next16 chain from the boot state 0 [orig: @ 0x6130a0].
		uint32_t s = water_noise_prng_step(0);
		if (!expect(s == 0x1u, "prng step 1")) return 1;
		s = water_noise_prng_step(s);
		if (!expect(s == 0x8011u, "prng step 2")) return 1;
		s = water_noise_prng_step(s);
		if (!expect(s == 0x40108111u, "prng step 3")) return 1;
		s = water_noise_prng_step(s);
		if (!expect(s == 0x4190B11Du, "prng step 4")) return 1;

		const WaterNoiseTables tables = water_init_noise_tables();
		// Sine LUT: 128 + 64*sin(2pi*i/256), truncating like the original ftol.
		if (!expect(tables.sine_lut[0] == 128 && tables.sine_lut[32] == 173 &&
		            tables.sine_lut[64] == 191 && tables.sine_lut[128] == 128 &&
		            tables.sine_lut[192] == 65, "sine LUT landmarks")) return 1;
		uint32_t lut_sum = 0;
		for (int i = 0; i < 256; ++i) lut_sum += tables.sine_lut[i];
		if (!expect(lut_sum == 32768u, "sine LUT sum (symmetry)")) return 1;
		// Field: deterministic from state 0.
		const uint8_t field_head[8] = {0, 127, 128, 176, 177, 225, 161, 184};
		for (int i = 0; i < 8; ++i) {
			if (!expect(tables.field[i] == field_head[i], "noise field head bytes")) return 1;
		}
		uint32_t field_sum = 0;
		for (int i = 0; i < kWaterNoiseSize * kWaterNoiseSize; ++i) field_sum += tables.field[i];
		if (!expect(field_sum == 2080878u, "noise field checksum")) return 1;

		// Color pass at counters 0 and 7: landmarks + wrapping checksums.
		static uint32_t color0[kWaterNoiseSize * kWaterNoiseSize];
		static uint32_t color7[kWaterNoiseSize * kWaterNoiseSize];
		water_noise_color_pixels(color0, tables, 0);
		water_noise_color_pixels(color7, tables, 7);
		if (color0[0] != 0xE17D7D7Du || color0[1] != 0xE7707070u ||
		    color0[64 * 128 + 64] != 0xE17C7C7Cu) {
			// Divergence diagnostics (macOS-runner red, env #35 follow-up): dump
			// the actual words plus a reconstruction of the t=0 intermediate
			// (animated[i] == sine_lut[field[i]]) so a single CI log separates
			// wrong-inputs from wrong-kernel.
			std::fprintf(stderr,
			             "diag: color0[0]=0x%08X color0[1]=0x%08X color0[center]=0x%08X\n",
			             color0[0], color0[1], color0[64 * 128 + 64]);
			uint32_t animated_sum = 0;
			for (int i = 0; i < kWaterNoiseSize * kWaterNoiseSize; ++i) {
				animated_sum += tables.sine_lut[tables.field[i]];
			}
			std::fprintf(stderr, "diag: t0 animated head =");
			for (int i = 0; i < 8; ++i) {
				std::fprintf(stderr, " %02X", tables.sine_lut[tables.field[i]]);
			}
			std::fprintf(stderr, " sum=%u\n", animated_sum);
			uint32_t field_sum_now = 0, lut_sum_now = 0;
			for (int i = 0; i < kWaterNoiseSize * kWaterNoiseSize; ++i) field_sum_now += tables.field[i];
			for (int i = 0; i < 256; ++i) lut_sum_now += tables.sine_lut[i];
			std::fprintf(stderr, "diag: field_sum=%u lut_sum=%u sizeof(tables)=%u\n",
			             field_sum_now, lut_sum_now,
			             static_cast<unsigned>(sizeof(WaterNoiseTables)));
			std::fprintf(stderr, "diag: color0 head words =");
			for (int i = 0; i < 4; ++i) std::fprintf(stderr, " %08X", color0[i]);
			std::fprintf(stderr, "\n");
			// Cross-check: recompute the whole t=0 pass naively here and
			// report the first mismatching pixels with their taps — separates
			// a lib miscompile from bad pins in one log.
			static uint8_t anim_ref[kWaterNoiseSize * kWaterNoiseSize];
			for (int i = 0; i < kWaterNoiseSize * kWaterNoiseSize; ++i) {
				anim_ref[i] = tables.sine_lut[tables.field[i]];
			}
			int reported = 0;
			for (int row = 0; row < kWaterNoiseSize && reported < 3; ++row) {
				for (int col = 0; col < kWaterNoiseSize && reported < 3; ++col) {
					const auto tap = [&](int r, int c) -> int {
						return anim_ref[((r & 0x7F) << 7) + (c & 0x7F)];
					};
					const int kernel =
							(3 * (tap(row - 1, col - 1) + tap(row - 1, col + 1) +
							      tap(row + 1, col - 1) + tap(row + 1, col + 1)) +
							 4 * (tap(row, col) + tap(row - 1, col) + tap(row + 1, col) +
							      tap(row, col - 1) + tap(row, col + 1))) >> 5;
					int inten = 128 - (kernel - 128 < 0 ? 128 - kernel : kernel - 128);
					if (inten < 0) inten = 0;
					const int ainv = (inten * inten) >> 9;
					const uint32_t want =
							(0x10101u * static_cast<uint32_t>(inten)) |
							(static_cast<uint32_t>(255 - ainv) << 24);
					const uint32_t got = color0[(row << 7) + col];
					if (got != want) {
						std::fprintf(stderr,
						             "diag: first mismatch @ (%d,%d) lib=%08X ref=%08X taps=%d %d %d %d / %d %d %d %d %d\n",
						             row, col, got, want,
						             tap(row - 1, col - 1), tap(row - 1, col + 1),
						             tap(row + 1, col - 1), tap(row + 1, col + 1),
						             tap(row, col), tap(row - 1, col), tap(row + 1, col),
						             tap(row, col - 1), tap(row, col + 1));
						++reported;
					}
				}
			}
			if (reported == 0) {
				std::fprintf(stderr, "diag: lib output matches the naive reference everywhere — the pins themselves mismatch\n");
			}
		}
		if (!expect(color0[0] == 0xE17D7D7Du && color0[1] == 0xE7707070u &&
		            color0[64 * 128 + 64] == 0xE17C7C7Cu, "color pixels landmarks (t=0)")) return 1;
		if (!expect(color7[0] == 0xE17C7C7Cu, "color pixel [0] (t=7)")) return 1;
		uint32_t sum0 = 0, sum7 = 0;
		for (int i = 0; i < kWaterNoiseSize * kWaterNoiseSize; ++i) { sum0 += color0[i]; sum7 += color7[i]; }
		if (!expect(sum0 == 0x45E0C3DCu, "color checksum (t=0)")) return 1;
		if (!expect(sum7 == 0x14D0B3D2u, "color checksum (t=7)")) return 1;

		// Normal/DuDv pass: B=0xFF, G/R = 2x saturated derivative + 0x80.
		static uint32_t normal0[kWaterNoiseSize * kWaterNoiseSize];
		water_noise_normal_pixels(normal0, color0);
		if (!expect(normal0[0] == 0x849CFFu && normal0[1] == 0x6666FFu,
		            "normal pixels landmarks")) return 1;
		uint32_t nsum = 0;
		for (int i = 0; i < kWaterNoiseSize * kWaterNoiseSize; ++i) nsum += normal0[i];
		if (!expect(nsum == 0x203FC000u, "normal checksum")) return 1;

		// UV state: scale/bias from the fog-distance INT part, offsets from
		// the layer-1 cloud accumulators + 32x camera (positive on both).
		CloudScrollState scroll;
		scroll.acc_l1_v = 61440;
		scroll.acc_l1_u = 61440;
		const WaterUvState uv = water_uv_state(scroll, 100.0f, 200.0f, 1024.0f);
		if (!expect(near(uv.scale, 1.0001649f, 1e-6f), "uv scale = 0.99996948*w/(w-0.2)")) return 1;
		if (!expect(near(uv.bias, 0.2000330f, 1e-6f), "uv bias = 0.2*scale")) return 1;
		if (!expect(near(uv.offset_u, 1.5627289f, 1e-6f), "uv offset u = cam_z/128 + acc*2^-28")) return 1;
		if (!expect(near(uv.offset_v, 0.7814789f, 1e-6f), "uv offset v = cam_x/128 + acc*2^-28")) return 1;
	}

	// --- Celestial bodies + glare occlusion [orig: render_celestial_bodies
	// @ 0x5acaa0; render_skybox_sun_glow @ 0x5acd00] ------------------------
	{
		if (!expect(kCelestialBodyDistance == 64.0f, "bodies place at camera + dir * 64")) return 1;

		// Sun alpha: (1 - overcast) x (100 - dim)/100 fold, 16.16.
		if (!expect(celestial_sun_alpha_fixed(0, 0) == 0x10000, "sun alpha full at clear defaults")) return 1;
		if (!expect(celestial_sun_alpha_fixed(0x8000, 0) == 0x8000, "half overcast halves the sun")) return 1;
		const int sun_dim_half = celestial_sun_alpha_fixed(0, 50 << 16);
		if (!expect(sun_dim_half >= 0x8000 && sun_dim_half <= 0x8001,
		            "SunDim 50 halves the sun (+1 fold rounding)")) return 1;
		if (!expect(celestial_sun_alpha_fixed(0x10000, 0) == 0, "full overcast hides the sun")) return 1;

		// Moon alpha: (fogInt - 400)/600 x (1 - overcast), clamped.
		if (!expect(celestial_moon_alpha_fixed(1024.0f, 0, false) == 0x10000, "moon full at fog 1024")) return 1;
		if (!expect(celestial_moon_alpha_fixed(400.0f, 0, false) == 0, "moon hidden at fog <= 400")) return 1;
		const int moon_700 = celestial_moon_alpha_fixed(700.0f, 0, false);
		if (!expect(moon_700 >= 0x7FFE && moon_700 <= 0x8001, "moon half at fog 700")) return 1;
		if (!expect(celestial_moon_alpha_fixed(1024.0f, 0x10000, false) == 0, "overcast hides the moon")) return 1;

		// Jitter pattern from the frame-index bits.
		const GlareRayJitter j0 = glare_ray_jitter(0);
		if (!expect(j0.offset_eng_y == -24.0f && j0.offset_eng_z == -16.0f,
		            "jitter 0 = (-16-8, -16)")) return 1;
		const GlareRayJitter j7 = glare_ray_jitter(7);
		if (!expect(j7.offset_eng_y == 24.0f && j7.offset_eng_z == 16.0f,
		            "jitter 7 = (+16+8, +16)")) return 1;
		const GlareRayJitter j5 = glare_ray_jitter(5);
		if (!expect(j5.offset_eng_y == 24.0f && j5.offset_eng_z == -16.0f,
		            "jitter 5 = (+16+8, -16)")) return 1;

		// The occlusion window + hysteresis: all-visible at fog 1000 ramps
		// toward 256 by 16/frame; going dark decays.
		GlareOcclusionState occ;
		for (int frame = 0; frame < 8; ++frame) {
			glare_occlusion_tick(occ, true, true, 1000.0f);
		}
		if (!expect(occ.window == 0xFF, "window fills after 4 frames of visible pairs")) return 1;
		if (!expect(occ.brightness == 128, "brightness ramped 16 x 8 frames")) return 1;
		for (int frame = 0; frame < 16; ++frame) {
			glare_occlusion_tick(occ, true, true, 1000.0f);
		}
		if (!expect(occ.brightness >= 240 && occ.brightness <= 256,
		            "brightness settles in the 256 dead-band")) return 1;
		for (int frame = 0; frame < 4; ++frame) {
			glare_occlusion_tick(occ, false, false, 1000.0f);
		}
		if (!expect(occ.window == 0x00, "window empties after 4 dark frames")) return 1;
		if (!expect(occ.brightness < 240, "brightness decays toward 0")) return 1;

		// Glow alpha: dot^4/2 x brightness x folds. Full-on = 0x8000 (the /2).
		if (!expect(glare_glow_alpha_fixed(0x10000, 256, 0, 0) == 0x8000,
		            "glow alpha caps at dot^4/2 full brightness")) return 1;
		if (!expect(glare_glow_alpha_fixed(0, 256, 0, 0) == 0, "glow off looking away")) return 1;
		const int glow_half_dot = glare_glow_alpha_fixed(0x8000, 256, 0, 0);
		if (!expect(glow_half_dot == 0x800, "glow at dot 0.5 = 0.5^4/2 = 1/32")) return 1;
		if (!expect(glare_glow_alpha_fixed(0x10000, 128, 0, 0) == 0x4000,
		            "brightness halves the glow")) return 1;
		if (!expect(glare_glow_alpha_fixed(0x10000, 256, 0x8000, 0) == 0x4000,
		            "overcast halves the glow")) return 1;
	}

	// --- Water strip tessellation (env #29) [orig: render_water_strip_detailed
	// @ 0x5c27d0; terrain_project_sector_to_screen @ 0x5c0bf0;
	// clip_line_to_viewport @ 0x5c0a30] --------------------------------------
	{
		// Stride pins: clamp(int(rhw * 500), 2, 9) [orig: @ 0x5c30c7..0x5c30eb].
		if (!expect(water_strip_stride(0.004f) == 2, "stride lands the low edge (0.004*500 = 2)")) return 1;
		if (!expect(water_strip_stride(0.001f) == 2, "stride clamps up to 2 (int(0.5) = 0)")) return 1;
		if (!expect(water_strip_stride(0.008f) == 4, "stride interior int(0.008*500) = 4")) return 1;
		// 0.018f is 0.0179999992...: ftol truncates 8.9999996 to 8 - the
		// witnessed truncation, not rounding.
		if (!expect(water_strip_stride(0.018f) == 8, "stride ftol truncates below the edge")) return 1;
		if (!expect(water_strip_stride(0.0181f) == 9, "stride lands the high edge (int(9.05) = 9)")) return 1;
		if (!expect(water_strip_stride(0.1f) == 9, "stride clamps down to 9")) return 1;

		// Depth ("fog W") clamps [orig: flt_7DBF7C / flt_7C4658 @ 0x5c2c1f..0x5c2c4a].
		if (!expect(water_strip_depth(0.1f, 1.0f, 0.2f) == kWaterStripDepthMin,
		            "depth clamps at the 8.042e-5 lower bound")) return 1;
		if (!expect(water_strip_depth(1.0e9f, 1.0001649f, 0.2f) == kWaterStripDepthMax,
		            "depth clamps at the 1 - 2^-14 upper bound")) return 1;
		if (!expect(near(water_strip_depth(100.0f, 1.0f, 0.2f), 0.998f, 1e-5f),
		            "interior depth follows (t*scale - bias)/t")) return 1;

		// The murk-angle chain at sin = 0.6 (a 3-4-5 right-edge ray), murk 0.5,
		// t 512 against fog end 1024 — hand-computed from the witnessed
		// constants: base 0.5, k = 0.2 + 0.8*0.5 = 0.6;
		// brightness = int(38.4k + (192k - 38.4k)*(1 - 0.6)) = int(59.904) = 59;
		// alpha_term = int(229.5*0.5*0.6) = int(68.85) = 68;
		// a = int(512 * 2^24 / (1024<<16)) = 128; dist = 255 - 128*128/255 = 191;
		// alpha = 68*191/255 = 50 -> diffuse 0x32 | 0x10101*0x3B.
		const float ray345[3] = {0.0f, -3.0f, 4.0f};
		const WaterRowColors murky = water_strip_row_colors(
				512.0f, ray345, 1024 << 16, 0.5f, 0x00804020u, false, false);
		if (!expect(murky.diffuse == 0x323B3B3Bu, "murk-chain diffuse bytes")) return 1;
		// spec term = int(128*0.5 + 127*0.5*(1 - 0.6)) = int(89.4) = 89:
		// R 128*89>>8 = 44, G 64*89>>8 = 22, B 32*89>>8 = 11, alpha = 191.
		if (!expect(murky.specular == 0xBF2C160Bu,
		            "specular = WaterColorLit x angle term >> 8 under the dist alpha")) return 1;

		// Underwater view: murk skipped (solid white diffuse) and the LINEAR
		// distance alpha (255 - a, no square) [orig: @ 0x5c2df3..0x5c2e20];
		// base 1 zeroes the specular RGB.
		const WaterRowColors under = water_strip_row_colors(
				512.0f, ray345, 1024 << 16, 0.5f, 0x00804020u, true, false);
		if (!expect(under.diffuse == 0xFFFFFFFFu, "underwater diffuse is solid white")) return 1;
		if (!expect(under.specular == 0x7F000000u,
		            "underwater specular carries only the linear 255 - a")) return 1;

		// Nightvision redraw: the flat 0.1 base [orig: flt_7C69F4 @ 0x5c2d5a]
		// and no specular RGB [orig: @ 0x5c2ef8]: k = 0.28,
		// brightness = int(0.28*(38.4 + 153.6*0.4)) = 27;
		// alpha = int(229.5*0.1*0.6) = 13 -> 13*191/255 = 9.
		const WaterRowColors nv = water_strip_row_colors(
				512.0f, ray345, 1024 << 16, 0.5f, 0x00804020u, false, true);
		if (!expect(nv.diffuse == 0x091B1B1Bu, "nightvision diffuse (0.1 base chain)")) return 1;
		if (!expect(nv.specular == 0xBF000000u, "nightvision drops the specular RGB")) return 1;

		// Screen block: identity-rotation view 100 units above the plane,
		// 640x480 viewport, proj m00 = m11 = 1 with w = view z.
		WaterStripView v{};
		v.view[0] = v.view[5] = v.view[10] = v.view[15] = 1.0f;
		v.view[13] = -100.0f; // camera at (0, 100, 0)
		v.view_inv[0] = v.view_inv[5] = v.view_inv[10] = v.view_inv[15] = 1.0f;
		v.view_inv[13] = 100.0f;
		v.proj[0] = 1.0f;
		v.proj[5] = 1.0f;
		v.proj[11] = 1.0f;
		v.cam_x_fp = 0;
		v.cam_y_fp = 100 << 16;
		v.cam_z_fp = 0;
		v.vp_min_x = 0;
		v.vp_min_y = 0;
		v.vp_max_x = 640;
		v.vp_max_y = 480;
		v.vp_center_x = 320;
		v.vp_center_y = 240;
		v.fog_end_fp = 1024 << 16;

		WaterScreenBlock block;
		water_project_plane_to_screen(v, 0, block);
		// Origin: (0, 0, 2000) -> view (0, -100, 2000), w 2000 ->
		// (320, 240 + (100/4000)*480) = (320, 252).
		if (!expect(near(block.origin[0], 320.0f) && near(block.origin[1], 252.0f),
		            "block origin at the 2000-unit plane point")) return 1;
		// Row delta: the 1000-unit RIGHT step -> +160 px; dy 0 forces the
		// witnessed literal 1e-6 [orig: @ 0x5c0deb].
		if (!expect(near(block.row_delta[0], 160.0f), "block row delta x")) return 1;
		if (!expect(block.row_delta[1] == 0.000001f, "block dy guard is the literal 1e-6")) return 1;
		// Reference point (0, 0, 1000) -> (320, 264); march = (0, 1).
		if (!expect(near(block.ref_point[0], 320.0f) && near(block.ref_point[1], 264.0f),
		            "block 1000-unit reference point")) return 1;
		if (!expect(near(block.march_dir[0], 0.0f) && near(block.march_dir[1], 1.0f),
		            "block march direction")) return 1;
		if (!expect(block.visible == 1 && block.origin_row_visible == 1,
		            "block visible with the origin in-viewport")) return 1;

		// Pitch the camera down (fwd = (0, -0.8, 0.6), a 3-4-5 pitch): the
		// origin projects above the screen (sy = -48.75), its row line
		// misses, but the march heads toward the viewport center - the
		// halfplane rescues it and the origin clamps onto the entry edges
		// [orig: @ 0x5c0f7b..0x5c1023].
		WaterStripView pitched = v;
		const float pitched_view[16] = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 0.6f, -0.8f, 0.0f,
			0.0f, 0.8f, 0.6f, 0.0f,
			0.0f, -60.0f, 80.0f, 1.0f,
		};
		for (int i = 0; i < 16; ++i) pitched.view[i] = pitched_view[i];
		WaterScreenBlock rescued;
		water_project_plane_to_screen(pitched, 0, rescued);
		if (!expect(rescued.origin_row_visible == 0 && rescued.visible == 1,
		            "halfplane rescues the off-screen origin")) return 1;
		if (!expect(rescued.origin[0] == 641.0f && rescued.origin[1] == 0.0f,
		            "origin clamps onto the entry edges (maxX+1, minY)")) return 1;
		if (!expect(near(rescued.march_dir[0], 0.0f) && near(rescued.march_dir[1], 1.0f),
		            "pitched march still heads down-screen")) return 1;

		// Looking up (fwd = (0, 0.8, 0.6)): the origin sits below the screen
		// and the march heads away from the center - invisible.
		WaterStripView skyward = v;
		const float skyward_view[16] = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 0.6f, 0.8f, 0.0f,
			0.0f, -0.8f, 0.6f, 0.0f,
			0.0f, -60.0f, -80.0f, 1.0f,
		};
		for (int i = 0; i < 16; ++i) skyward.view[i] = skyward_view[i];
		WaterScreenBlock away;
		water_project_plane_to_screen(skyward, 0, away);
		if (!expect(away.visible == 0, "halfplane rejects a strip marching away")) return 1;

		// Clip: a diagonal through the center (dx/dy = 1) pins the two-stage
		// clamp arithmetic exactly; seeds (0, -80)/(641, 561) clamp onto the
		// top and bottom edges.
		WaterRowClip rc;
		water_clip_row_to_viewport(v, 320.0f, 240.0f, 1.0f, rc);
		if (!expect(rc.crossed, "diagonal row line crosses")) return 1;
		if (!expect(rc.left[0] == 80.0f && rc.left[1] == 0.0f,
		            "clip left endpoint clamps to the top edge")) return 1;
		if (!expect(rc.right[0] == 561.0f && rc.right[1] == 481.0f,
		            "clip right endpoint clamps to the bottom edge")) return 1;
		// A near-horizontal line through the center spans edge to edge.
		water_clip_row_to_viewport(v, 320.0f, 240.0f, 2.5e8f, rc);
		if (!expect(rc.crossed && rc.left[0] == 0.0f && rc.right[0] == 641.0f &&
		            near(rc.left[1], 240.0f) && near(rc.right[1], 240.0f),
		            "horizontal row line spans the viewport")) return 1;
		// A line fully below the viewport clamps out of x-range: no crossing.
		water_clip_row_to_viewport(v, 320.0f, 1000.0f, 2.5e8f, rc);
		if (!expect(!rc.crossed, "row line below the viewport does not cross")) return 1;

		// The march loop: row 0 spans the viewport at y ~252 and unprojects
		// to the plane at view depth ~2000 (ray (-1, -0.05, 1)); the next row
		// advances by the stride floor (rhw*500 = 0.25 -> 2).
		WaterStripParams sp;
		sp.plane_height_fp = 0;
		sp.water_murk = 0.5f;
		sp.water_color_lit = 0x00804020u;
		sp.uv_scale = 1.0f;
		sp.uv_bias = 0.2f;
		WaterStripRows rows;
		const int count = water_build_strip_rows(v, sp, rows);
		if (!expect(count >= 2, "level view emits rows")) return 1;
		if (!expect(static_cast<int>(rows.screen_pos.size()) == count * 6 &&
		            static_cast<int>(rows.depth.size()) == count * 3 &&
		            static_cast<int>(rows.rhw.size()) == count * 3 &&
		            static_cast<int>(rows.diffuse.size()) == count * 3 &&
		            static_cast<int>(rows.specular.size()) == count * 3 &&
		            static_cast<int>(rows.uv0.size()) == count * 6 &&
		            static_cast<int>(rows.t1.size()) == count * 9 &&
		            static_cast<int>(rows.t2.size()) == count * 9,
		            "row vectors hold 3 vertices per row")) return 1;
		if (!expect(rows.screen_pos[0] == 0.0f && near(rows.screen_pos[1], 252.0f),
		            "row0 left vertex screen position")) return 1;
		if (!expect(near(rows.rhw[0], 0.0005f, 1e-7f), "row0 left rhw = 1/2000")) return 1;
		// World (-2000, 0, 2000) -> uv0 = world/32 [orig: flt_7DBFAC].
		if (!expect(near(rows.uv0[0], -62.5f, 1e-3f) && near(rows.uv0[1], 62.5f, 1e-3f),
		            "row0 left uv0 = world / 32")) return 1;
		// depth = (2000*1 - 0.2)/2000 = 0.9999.
		if (!expect(near(rows.depth[0], 0.9999f, 1e-6f), "row0 depth curve")) return 1;
		// Diffuse/specular are row-constant [orig: @ 0x5c2f0a..0x5c2f2b].
		if (!expect(rows.diffuse[0] == rows.diffuse[1] && rows.diffuse[1] == rows.diffuse[2],
		            "diffuse is row-constant")) return 1;
		if (!expect(rows.specular[0] == rows.specular[1] && rows.specular[1] == rows.specular[2],
		            "specular is row-constant")) return 1;
		if (!expect(near(rows.screen_pos[7], 254.0f, 1e-3f),
		            "row1 marches 2 px (the stride floor)")) return 1;

		// Every emitted vertex must reproject to the screen coordinate carried
		// by its texm3x2 row. This pins the reimpl extension of the witnessed
		// centered-perspective march to off-center perspective and orthographic
		// projections without weakening the retail fixture above.
		const auto row_vertex_reprojects = [&](const WaterStripView &test_view,
				const WaterStripRows &test_rows, size_t vertex) {
			const float world[3] = {
				test_rows.uv0[vertex * 2] * 32.0f,
				0.0f,
				test_rows.uv0[vertex * 2 + 1] * 32.0f,
			};
			const float *m = test_view.view;
			const float vx = m[0] * world[0] + m[4] * world[1] + m[8] * world[2] + m[12];
			const float vy = m[1] * world[0] + m[5] * world[1] + m[9] * world[2] + m[13];
			const float vz = m[2] * world[0] + m[6] * world[1] + m[10] * world[2] + m[14];
			const float *p = test_view.proj;
			const float cx = p[0] * vx + p[4] * vy + p[8] * vz + p[12];
			const float cy = p[1] * vx + p[5] * vy + p[9] * vz + p[13];
			const float cw = p[3] * vx + p[7] * vy + p[11] * vz + p[15];
			const float sx = test_view.vp_center_x + cx / (2.0f * cw) *
					(test_view.vp_max_x - test_view.vp_min_x);
			const float sy = test_view.vp_center_y - cy / (2.0f * cw) *
					(test_view.vp_max_y - test_view.vp_min_y);
			return near(sx, test_rows.screen_pos[vertex * 2], 2e-3f) &&
					near(sy, test_rows.screen_pos[vertex * 2 + 1], 2e-3f);
		};

		WaterStripView off_center = v;
		// Converted Godot frustum projection: clip x = m00*x - m20*z,
		// clip y = m11*y - m21*z, clip w = +view depth.
		off_center.proj[8] = -0.20f;
		off_center.proj[9] = 0.10f;
		WaterStripRows off_center_rows;
		const int off_center_count = water_build_strip_rows(off_center, sp, off_center_rows);
		if (!expect(off_center_count >= 2, "off-center perspective emits rows")) return 1;
		if (!expect(row_vertex_reprojects(off_center, off_center_rows, 0) &&
		            row_vertex_reprojects(off_center, off_center_rows,
		                                  off_center_rows.uv0.size() / 2 - 1),
		            "off-center perspective rows invert z-shear projection")) return 1;

		// Orthographic camera pitched down at the plane. Clip W is constant;
		// each screen pixel owns a different line origin while every ray shares
		// the camera forward direction.
		WaterStripView orthographic = v;
		const float ortho_view[16] = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 0.6f, -0.8f, 0.0f,
			0.0f, 0.8f, 0.6f, 0.0f,
			0.0f, -60.0f, 80.0f, 1.0f,
		};
		const float ortho_inv[16] = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 0.6f, 0.8f, 0.0f,
			0.0f, -0.8f, 0.6f, 0.0f,
			0.0f, 100.0f, 0.0f, 1.0f,
		};
		for (int i = 0; i < 16; ++i) {
			orthographic.view[i] = ortho_view[i];
			orthographic.view_inv[i] = ortho_inv[i];
			orthographic.proj[i] = 0.0f;
		}
		orthographic.proj[0] = 1.0f / 320.0f; // 640-world-unit width
		orthographic.proj[5] = 1.0f / 240.0f; // 480-world-unit height
		orthographic.proj[15] = 1.0f;          // constant clip W
		orthographic.cam_right[0] = 1.0f;
		orthographic.cam_right[1] = 0.0f;
		orthographic.cam_right[2] = 0.0f;
		orthographic.cam_forward[0] = 0.0f;
		orthographic.cam_forward[1] = -0.8f;
		orthographic.cam_forward[2] = 0.6f;
		WaterStripRows ortho_rows;
		const int ortho_count = water_build_strip_rows(orthographic, sp, ortho_rows);
		if (!expect(ortho_count >= 2, "orthographic pitched view emits rows")) return 1;
		if (!expect(row_vertex_reprojects(orthographic, ortho_rows, 0) &&
		            row_vertex_reprojects(orthographic, ortho_rows,
		                                  ortho_rows.uv0.size() / 2 - 1),
		            "orthographic rows invert constant-W projection")) return 1;
		if (!expect(near(ortho_rows.rhw[0], 1.0f),
		            "orthographic row RHW is reciprocal constant clip W")) return 1;
		if (!expect(near(ortho_rows.screen_pos[7] - ortho_rows.screen_pos[1], 9.0f, 1e-3f),
		            "orthographic constant RHW drives the clamped 9-pixel stride")) return 1;
		if (!expect(near(ortho_rows.uv0[4] - ortho_rows.uv0[0], 20.03125f, 2e-3f),
		            "orthographic row spans the viewport at constant world scale")) return 1;

		// The underwater pass: white diffuse, the boot stride 4 (never
		// re-derived [orig: @ 0x5c30c5]) and the flipped t2 V
		// [orig: @ 0x5c306f..0x5c3085].
		WaterStripParams under_params = sp;
		under_params.underwater_view = true;
		WaterStripRows under_rows;
		if (!expect(water_build_strip_rows(v, under_params, under_rows) >= 2,
		            "underwater build emits rows")) return 1;
		if (!expect(under_rows.diffuse[0] == 0xFFFFFFFFu, "underwater diffuse white")) return 1;
		if (!expect(near(under_rows.screen_pos[7], 256.0f, 1e-3f),
		            "underwater keeps the boot stride 4")) return 1;
		if (!expect(near(under_rows.t2[2], 1.0f - rows.t2[2], 1e-6f),
		            "underwater flips t2's V to 1 - V")) return 1;

		// The 1024-row cap [orig: cmp 0x400 @ 0x5c312d]: a tall viewport
		// keeps the march crossing for thousands of rows.
		WaterStripView tall = v;
		tall.vp_min_x = 0;
		tall.vp_min_y = 0;
		tall.vp_max_x = 1024;
		tall.vp_max_y = 65536;
		tall.vp_center_x = 512;
		tall.vp_center_y = 32768;
		WaterStripRows capped;
		if (!expect(water_build_strip_rows(tall, sp, capped) == kWaterStripMaxRows,
		            "the march stops at the 1024-row cap")) return 1;
		if (!expect(static_cast<int>(capped.depth.size()) == kWaterStripMaxRows * 3,
		            "capped vectors sized to the cap")) return 1;

		// Batch table [orig: @ 0x5c313f..0x5c329e]: <=5-row windows stepping
		// 4 with 8n-10 vertices and 8n-12 strip primitives (DrawPrimitive
		// passes vertex_count - 2 @ 0x5c3209).
		if (!expect(water_strip_batches(0).empty() && water_strip_batches(1).empty(),
		            "no batches under 2 rows")) return 1;
		std::vector<WaterStripBatch> batches = water_strip_batches(2);
		if (!expect(batches.size() == 1 && batches[0].first_row == 0 && batches[0].rows == 2 &&
		            batches[0].vertex_count == 6 && batches[0].primitive_count == 4,
		            "2 rows -> one 6-vertex window")) return 1;
		batches = water_strip_batches(5);
		if (!expect(batches.size() == 1 && batches[0].rows == 5 &&
		            batches[0].vertex_count == 30 && batches[0].primitive_count == 28,
		            "5 rows -> one 30-vertex strip")) return 1;
		batches = water_strip_batches(6);
		if (!expect(batches.size() == 2 && batches[1].first_row == 4 && batches[1].rows == 2,
		            "6 rows overlap 1 row into a trailing window")) return 1;
		batches = water_strip_batches(9);
		if (!expect(batches.size() == 2 && batches[0].first_row == 0 && batches[0].rows == 5 &&
		            batches[1].first_row == 4 && batches[1].rows == 5,
		            "9 rows -> windows [0..4] and [4..8]")) return 1;
		batches = water_strip_batches(12);
		if (!expect(batches.size() == 3 && batches[2].first_row == 8 && batches[2].rows == 4 &&
		            batches[2].vertex_count == 22 && batches[2].primitive_count == 20,
		            "12 rows -> three windows ending in a 4-row one")) return 1;
		for (int n = 1; n <= 12; ++n) {
			for (const WaterStripBatch &batch : water_strip_batches(n)) {
				if (batch.rows < 2 || batch.rows > 5 || batch.first_row % 4 != 0 ||
				    batch.vertex_count != 8 * batch.rows - 10 ||
				    batch.primitive_count != batch.vertex_count - 2 ||
				    batch.first_row + batch.rows > n) {
					std::fprintf(stderr, "FAIL: batch window shape at n=%d\n", n);
					return 1;
				}
			}
		}
		// The static strip index table [orig: word_841328]: row-pair blocks
		// joined by degenerate stitches; a batch of n rows reads 8n-10 entries.
		const uint16_t index_head[8] = {3, 0, 4, 1, 5, 2, 2, 6};
		for (int i = 0; i < 8; ++i) {
			if (!expect(kWaterStripIndexTable[i] == index_head[i],
			            "witnessed strip index head")) return 1;
		}
		if (!expect(kWaterStripIndexTable[28] == 14 && kWaterStripIndexTable[29] == 11,
		            "witnessed strip index tail")) return 1;
	}

	std::printf(
	    "OK: env_render fog/day-phase/smoothing/lightning/glare/overrides/horizon/tint/iris"
	    "/oscillator/sequencers/blocks/scroll/dome/waternoise/celestial/waterstrip\n");
	return 0;
}
