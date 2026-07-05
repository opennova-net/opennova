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

	std::printf(
	    "OK: env_render fog/day-phase/smoothing/lightning/glare/overrides/horizon/tint/iris\n");
	return 0;
}
