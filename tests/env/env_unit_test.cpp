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

	const opennova::env::TodState between = opennova::env::interpolate_tod(loaded.keyframes, 1300.0f, loaded.envscale);
	if (!expect(near(between.sun.r, 167.5f / 255.0f), "TOD interpolation should blend between 1200 and 1400")) return 1;

	const opennova::env::TodState wrap = opennova::env::interpolate_tod(loaded.keyframes, 100.0f, loaded.envscale);
	if (!expect(wrap.moon.r > 0.0f, "TOD interpolation should wrap across midnight")) return 1;

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
