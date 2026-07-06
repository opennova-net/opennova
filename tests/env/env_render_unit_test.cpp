// Engine-faithful atmosphere math, asserted against the exact fixed-point
// behavior witnessed in Jointops.exe. RE record: docs/env/env-tod-re.md.
#include <env/env_render.h>

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

		if (!expect(glare_brightness_step(0, 8) == 16, "brightness rises 16/frame")) return 1;
		if (!expect(glare_brightness_step(248, 8) == 255, "brightness clamps at 255")) return 1;
		if (!expect(glare_brightness_step(100, 0) == 84, "brightness falls 16/frame")) return 1;
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

		// Foliage lightmap tint [orig: sample_terrain_lightmap @ 0x606030]:
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

	std::printf(
	    "OK: env_render fog/day-phase/smoothing/lightning/glare/overrides/horizon/tint/iris"
	    "/oscillator/sequencers/blocks/scroll/dome\n");
	return 0;
}
