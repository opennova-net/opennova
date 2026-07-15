#include <env/env.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near(float actual, float expected, float epsilon = 0.0001f) {
	return std::fabs(actual - expected) <= epsilon;
}

std::string fixture_path() {
#ifdef OPENNOVA_SOURCE_DIR
	return std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/env/full_00.env";
#else
	return "fixtures/env/full_00.env";
#endif
}

} // namespace

int main() {
	std::ifstream fixture(fixture_path(), std::ios::binary);
	if (!fixture) {
		std::fprintf(stderr, "FAIL: cannot open %s\n", fixture_path().c_str());
		return 1;
	}

	opennova::env::Config loaded;
	std::string error;
	if (!opennova::env::load_env(fixture, loaded, error)) {
		std::fprintf(stderr, "FAIL: load_env failed: %s\n", error.c_str());
		return 1;
	}

	if (!expect(loaded.name == "Full_00", "env name should parse")) return 1;
	if (!expect(loaded.timeofday == "Day", "timeofday should parse")) return 1;
	if (!expect(loaded.curtime == 1200, "curtime should parse")) return 1;
	if (!expect(near(loaded.fog_level, 1000.0f), "fog_level should parse")) return 1;
	if (!expect(loaded.fog_type == 2, "fog_type should parse")) return 1;
	if (!expect(!loaded.water_height_set, "full_00 does not author water_height")) return 1;
	if (!expect(loaded.sky_map1 == "Cloud01.pcx", "sky_map1 should parse")) return 1;
	if (!expect(loaded.sky_map2 == "Cloud01b.pcx", "sky_map2 should parse")) return 1;
	if (!expect(loaded.star_3di.empty(), "blank star_3di should parse as empty")) return 1;
	if (!expect(loaded.keyframes.size() == 10, "full_00 should have 10 TOD keyframes")) return 1;
	if (!expect(loaded.keyframes.front().time == 200, "first TOD keyframe should be 0200")) return 1;
	if (!expect(loaded.keyframes.back().time == 2359, "last TOD keyframe should be 2359")) return 1;
	if (!expect(near(loaded.vertex_rgb.r, 128.0f / 255.0f), "vertex_rgb should parse")) return 1;

	const opennova::env::TodState noon = opennova::env::interpolate_tod(loaded.keyframes, 1200.0f, loaded.envscale);
	if (!expect(near(noon.sun.r, 170.0f / 255.0f), "TOD exact noon sun should match keyframe")) return 1;
	if (!expect(near(noon.fog.b, 138.0f / 255.0f), "TOD exact noon fog should match keyframe")) return 1;

	// Integer channel lerp rounds half up: 1300 sits exactly between 1200 and
	// 1400 in HOURS space; sun.r 170 -> 165 lands on 168 (not 167.5).
	// [orig: Color_InterpolateRGB888 @ 0x57c2f0]
	const opennova::env::TodState between = opennova::env::interpolate_tod(loaded.keyframes, 1300.0f, loaded.envscale);
	if (!expect(near(between.sun.r, 168.0f / 255.0f), "TOD interpolation should blend between 1200 and 1400")) return 1;

	const opennova::env::TodState wrap = opennova::env::interpolate_tod(loaded.keyframes, 100.0f, loaded.envscale);
	if (!expect(wrap.moon.r > 0.0f, "TOD interpolation should wrap across midnight")) return 1;

	// The float getters expose retail's fixed tuples directly; normalizing here
	// perturbs the bytes packed later by the terrain and sky lighting paths.
	{
		const opennova::env::Vec3 sun = opennova::env::compute_sun_direction(1200.0f);
		const opennova::env::Vec3 moon = opennova::env::compute_moon_direction(0.0f);
		if (!expect(near(sun.x, -22414.0f / 65536.0f, 1.0e-7f), "sun fixed x tuple should stay unnormalized")) return 1;
		if (!expect(near(sun.y, 61583.0f / 65536.0f, 1.0e-7f), "sun fixed magnitude should stay unnormalized")) return 1;
		if (!expect(near(moon.x, -32768.0f / 65536.0f, 1.0e-7f), "moon fixed x tuple should stay unnormalized")) return 1;
		if (!expect(near(moon.y, 56755.0f / 65536.0f, 1.0e-7f), "moon fixed magnitude should stay unnormalized")) return 1;
	}

	// Interpolation runs in HOURS space, not HHMM: 1230 is halfway between 1200
	// and 1300 (12.5h), not 30% of the way. [orig: Environment_ParseTimeString @ 0x57c500]
	{
		std::vector<opennova::env::Keyframe> hours_frames(2);
		hours_frames[0].time = 1200;
		hours_frames[0].sun = {0.0f, 0.0f, 0.0f};
		hours_frames[1].time = 1300;
		hours_frames[1].sun = {200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f};
		const opennova::env::TodState half = opennova::env::interpolate_tod(hours_frames, 1230.0f, 1.0f);
		if (!expect(near(half.sun.r, 100.0f / 255.0f), "1230 should be the hours-space midpoint of 1200..1300")) return 1;

		// Fractions above 63356/65536 snap to 1.0 (original source typo kept for
		// parity): 12:59 of a one-hour segment yields 64443 > 63356 -> endpoint.
		// [orig: Environment_LerpKeyframeSet @ 0x57c3c6]
		const opennova::env::TodState snap = opennova::env::interpolate_tod(hours_frames, 1259.0f, 1.0f);
		if (!expect(near(snap.sun.r, 200.0f / 255.0f), "fraction above 63356 should snap to the end keyframe")) return 1;
	}

	// envscale quantizes at the byte level with truncation before the lerp.
	// [orig: Color_ScaleRGBAndPack @ 0x57f890]
	{
		std::vector<opennova::env::Keyframe> scale_frames(1);
		scale_frames[0].time = 0;
		scale_frames[0].sun = {170.0f / 255.0f, 170.0f / 255.0f, 170.0f / 255.0f};
		const opennova::env::TodState scaled = opennova::env::interpolate_tod(scale_frames, 0.0f, 1.1f);
		if (!expect(near(scaled.sun.r, 187.0f / 255.0f), "envscale should truncate at the byte level (170*1.1 = 187)")) return 1;
	}

	// The engine caps TOD keyframes at 16; a 17th block's colors bleed into the
	// 16th slot and its time is dropped. [orig: TimeOfDay_ParseProperty @ 0x57c65b]
	{
		std::ostringstream many;
		for (int i = 0; i < 17; ++i) {
			many << "tod_begin " << (100 * (i % 24)) << "\r\n";
			many << "    sun_rgb " << (i + 1) << "," << (i + 1) << "," << (i + 1) << "\r\n";
			many << "tod_end\r\n";
		}
		std::istringstream many_input(many.str());
		opennova::env::Config many_cfg;
		error.clear();
		if (!opennova::env::load_env(many_input, many_cfg, error)) {
			std::fprintf(stderr, "FAIL: 17-block env should parse: %s\n", error.c_str());
			return 1;
		}
		if (!expect(many_cfg.keyframes.size() == 16, "keyframes should cap at 16")) return 1;
		bool bleed_found = false;
		for (const opennova::env::Keyframe &kf : many_cfg.keyframes) {
			if (kf.time == 1500 && near(kf.sun.r, 17.0f / 255.0f)) {
				bleed_found = true;
			}
		}
		if (!expect(bleed_found, "17th block colors should bleed into the 16th slot, keeping its time")) return 1;
	}

	// curtime sanitizes digits positionally (hours <= 23, minutes <= 59).
	// [orig: Environment_ParseTimeString @ 0x57c500]
	{
		std::istringstream curtime_input("curtime 1290\r\n");
		opennova::env::Config curtime_cfg;
		error.clear();
		if (!opennova::env::load_env(curtime_input, curtime_cfg, error)) {
			std::fprintf(stderr, "FAIL: curtime env should parse: %s\n", error.c_str());
			return 1;
		}
		if (!expect(curtime_cfg.curtime == 1259, "curtime 1290 should sanitize to 1259")) return 1;
	}

	// Engine pre-parse defaults (what an .env omitting keywords means).
	// [orig: Environment_InitDefaults @ 0x57c010]
	{
		const opennova::env::Config engine_defaults;
		if (!expect(engine_defaults.sky_map1 == "cld_day1.pcx", "default sky_map1 is cld_day1.pcx")) return 1;
		if (!expect(engine_defaults.star_3di == "mstar.3di", "default star_3di is mstar.3di")) return 1;
		if (!expect(engine_defaults.fog_type == 1, "default fog_type is 1")) return 1;
		if (!expect(near(engine_defaults.fog_level, 1024.0f), "default fog_level is 1024")) return 1;
		if (!expect(near(engine_defaults.iris_percent, 50.0f), "default iris_percent is 50")) return 1;
		if (!expect(engine_defaults.advanced_clouds == 0, "default advanced_clouds is 0")) return 1;
		if (!expect(engine_defaults.curtime == 1500, "default curtime is 15:00")) return 1;
	}

	std::ostringstream saved_stream;
	error.clear();
	if (!opennova::env::save_env(saved_stream, loaded, error)) {
		std::fprintf(stderr, "FAIL: save_env failed: %s\n", error.c_str());
		return 1;
	}
	const std::string saved = saved_stream.str();
	if (!expect(saved.find("\r\n") != std::string::npos, "writer should use CRLF")) return 1;
	for (size_t i = 0; i < saved.size(); ++i) {
		if (saved[i] == '\n' && (i == 0 || saved[i - 1] != '\r')) {
			std::fprintf(stderr, "FAIL: writer produced bare LF at byte %zu\n", i);
			return 1;
		}
	}
	if (!expect(saved.find("tod_begin 0200\r\n") != std::string::npos, "writer should zero-pad TOD times")) return 1;

	opennova::env::Config reparsed;
	std::istringstream saved_input(saved);
	error.clear();
	if (!opennova::env::load_env(saved_input, reparsed, error)) {
		std::fprintf(stderr, "FAIL: reparsing saved env failed: %s\n", error.c_str());
		return 1;
	}
	if (!expect(reparsed.name == loaded.name, "name should roundtrip semantically")) return 1;
	if (!expect(reparsed.keyframes.size() == loaded.keyframes.size(), "keyframe count should roundtrip semantically")) return 1;
	if (!expect(reparsed.keyframes[5].time == 1200, "keyframe times should roundtrip semantically")) return 1;
	if (!expect(near(reparsed.keyframes[5].skyfog.b, loaded.keyframes[5].skyfog.b), "keyframe colors should roundtrip semantically")) return 1;

	const opennova::env::Config defaults = opennova::env::make_default_config();
	std::ostringstream default_stream;
	if (!opennova::env::save_env(default_stream, defaults, error)) {
		std::fprintf(stderr, "FAIL: save_env defaults failed: %s\n", error.c_str());
		return 1;
	}
	std::istringstream default_input(default_stream.str());
	opennova::env::Config default_reparsed;
	if (!opennova::env::load_env(default_input, default_reparsed, error)) {
		std::fprintf(stderr, "FAIL: default env should be reloadable: %s\n", error.c_str());
		return 1;
	}
	if (!expect(!default_reparsed.keyframes.empty(), "default env should include TOD keyframes")) return 1;

	std::istringstream water_input("water_height 32\r\nwater_murk 1.25\r\n");
	opennova::env::Config water_cfg;
	error.clear();
	if (!opennova::env::load_env(water_input, water_cfg, error)) {
		std::fprintf(stderr, "FAIL: water env should parse: %s\n", error.c_str());
		return 1;
	}
	if (!expect(water_cfg.water_height_set, "water_height should record authored presence")) return 1;
	if (!expect(near(water_cfg.water_height, 32.0f), "water_height should parse as world units")) return 1;
	if (!expect(near(water_cfg.water_murk, 0.99f), "water_murk should clamp at 0.99")) return 1;

	std::ostringstream water_saved;
	if (!opennova::env::save_env(water_saved, water_cfg, error)) {
		std::fprintf(stderr, "FAIL: save_env water failed: %s\n", error.c_str());
		return 1;
	}
	if (!expect(water_saved.str().find("water_height 32\r\n") != std::string::npos,
	            "writer should preserve authored water_height")) return 1;

	std::printf("OK: env parse/write/interpolation\n");
	return 0;
}
