// Generator + guard for fixtures/env: the synthetic environment file
// synth_full.env (every keyword authored, ten time-of-day keyframes; written
// by save_env from integer colour bytes) and the two sky maps it names
// (sky_map1 Cloud01.pcx, "full range cloud image, square rooted (bright
// gamma)"; sky_map2 Cloud01b.pcx, "subtile modulation layer, centered at
// 128": 64x64 indexed PCX files minted by encode_pcx_indexed from integer
// data). The env / sky dome / weather tests load the .env, resolve the maps
// beside it through the shared texture resolver and edit the bindings; no
// retail file is carried (the shipped .env set is the gated env_jo_install
// sweep).
//
// Default: rebuild in memory and byte-compare the committed files. `--write`
// (re)writes them.
#include "common/test_paths.h"

#include <formats/env/env.h>
#include <formats/pcx/pcx.h>
#include <formats/pcx/pcx_io.h>

#include <sstream>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using opennova::IndexedImage8;

#include "common/file_io.h"

namespace {

constexpr int kSize = 64;

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

// A grayscale ramp palette: index == luminance.
void gray_palette(IndexedImage8 &image) {
	for (int i = 0; i < 256; ++i) {
		image.palette[i][0] = static_cast<uint8_t>(i);
		image.palette[i][1] = static_cast<uint8_t>(i);
		image.palette[i][2] = static_cast<uint8_t>(i);
	}
}

// Two overlapping integer-phase sines, square rooted into the bright gamma
// the retail comment describes: a full-range cloud field with soft lobes.
IndexedImage8 make_cloud_map() {
	IndexedImage8 image;
	image.width = kSize;
	image.height = kSize;
	image.indices.resize(static_cast<size_t>(kSize) * kSize);
	gray_palette(image);
	for (int y = 0; y < kSize; ++y) {
		for (int x = 0; x < kSize; ++x) {
			const double u = static_cast<double>(x) / kSize * 6.283185307179586;
			const double v = static_cast<double>(y) / kSize * 6.283185307179586;
			const double field = 0.5 + 0.25 * std::sin(u * 2.0 + v) + 0.25 * std::sin(v * 3.0 - u * 0.5);
			const double bright = std::sqrt(field < 0.0 ? 0.0 : field);
			const int value = static_cast<int>(std::lround(bright * 255.0));
			image.indices[static_cast<size_t>(y) * kSize + x] =
					static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
		}
	}
	return image;
}

// The modulation layer: a low-amplitude ripple centered at 128.
IndexedImage8 make_modulation_map() {
	IndexedImage8 image;
	image.width = kSize;
	image.height = kSize;
	image.indices.resize(static_cast<size_t>(kSize) * kSize);
	gray_palette(image);
	for (int y = 0; y < kSize; ++y) {
		for (int x = 0; x < kSize; ++x) {
			const double u = static_cast<double>(x) / kSize * 6.283185307179586;
			const double v = static_cast<double>(y) / kSize * 6.283185307179586;
			const double ripple = 24.0 * std::sin(u * 4.0) * std::cos(v * 4.0);
			image.indices[static_cast<size_t>(y) * kSize + x] =
					static_cast<uint8_t>(128 + static_cast<int>(std::lround(ripple)));
		}
	}
	return image;
}

opennova::env::Rgb rgb(int r, int g, int b) {
	return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f};
}

// One keyframe from colour bytes, in the .env keyword order.
opennova::env::Keyframe keyframe(int time, opennova::env::Rgb sun, opennova::env::Rgb moon, opennova::env::Rgb sky,
                                 opennova::env::Rgb ground, opennova::env::Rgb skyfog, opennova::env::Rgb fog,
                                 opennova::env::Rgb skybase, opennova::env::Rgb skybright,
                                 opennova::env::Rgb skyhighlight, opennova::env::Rgb cloudbase,
                                 opennova::env::Rgb cloudhighlight, opennova::env::Rgb cloudedge) {
	opennova::env::Keyframe k;
	k.time = time;
	k.sun = sun;
	k.moon = moon;
	k.sky = sky;
	k.ground = ground;
	k.skyfog = skyfog;
	k.fog = fog;
	k.skybase = skybase;
	k.skybright = skybright;
	k.skyhighlight = skyhighlight;
	k.cloudbase = cloudbase;
	k.cloudhighlight = cloudhighlight;
	k.cloudedge = cloudedge;
	return k;
}

// The synthetic day: every top-level keyword authored (no water_height, an
// empty star object), ten keyframes from a dark small-hours pair through dawn,
// a flat midday plateau, dusk and back to midnight. The pins the env tests key
// on live here: curtime 1200 sits on a keyframe; noon and 1400 carry sun.r 170
// and 165 so the 1300 lerp lands on the half-up rounding case (168); the
// 2359/0200 pair carries a lit moon so a wrap across midnight interpolates it;
// the noon skyfog blue exceeds 127 so the doubled render fog saturates.
opennova::env::Config make_synth_env() {
	opennova::env::Config cfg;
	cfg.name = "Synth_Full";
	cfg.timeofday = "Day";
	cfg.envscale = 1.0f;
	cfg.iris_percent = 15.0f;
	cfg.iris_center = 1.0f;
	cfg.water_rgb = rgb(48, 64, 40);
	cfg.sky_map1 = "Cloud01.pcx";
	cfg.sky_map2 = "Cloud01b.pcx";
	cfg.sky_height = 160.0f;
	cfg.sky_speed = 12.0f;
	cfg.fog_level = 1000.0f;
	cfg.fog_type = 2;
	cfg.cloud_rgb = rgb(128, 128, 128);
	cfg.terrain_rgb = rgb(255, 255, 255);
	cfg.vertex_rgb = rgb(128, 128, 128);
	cfg.lightning_rgb = rgb(80, 80, 96);
	cfg.star_3di.clear();
	cfg.ceiling_rgb = rgb(56, 56, 56);
	cfg.floor_rgb = rgb(24, 24, 24);
	cfg.curtime = 1200;
	cfg.advanced_clouds = 1;
	const opennova::env::Rgb black = rgb(0, 0, 0);
	const opennova::env::Rgb night_moon = rgb(48, 64, 88);
	const opennova::env::Rgb night_sky = rgb(32, 36, 60);
	const opennova::env::Rgb night_fog = rgb(2, 2, 6);
	cfg.keyframes = {
	    keyframe(200, black, night_moon, night_sky, rgb(16, 28, 44), night_fog, night_fog, rgb(0, 1, 2), rgb(64, 64, 84),
	             rgb(2, 4, 2), rgb(14, 14, 20), rgb(52, 52, 52), rgb(44, 50, 60)),
	    keyframe(400, black, night_moon, night_sky, rgb(18, 36, 54), night_fog, night_fog, rgb(0, 1, 2), rgb(64, 64, 84),
	             rgb(2, 4, 2), rgb(14, 14, 20), rgb(52, 52, 52), rgb(44, 50, 60)),
	    keyframe(600, rgb(88, 72, 36), black, rgb(64, 76, 108), rgb(36, 36, 32), rgb(128, 100, 68), rgb(128, 100, 68),
	             rgb(8, 12, 28), rgb(6, 6, 20), rgb(224, 248, 96), rgb(44, 28, 30), rgb(6, 8, 4), rgb(152, 176, 84)),
	    keyframe(800, rgb(164, 164, 160), black, rgb(84, 84, 88), rgb(40, 44, 40), rgb(76, 92, 140), rgb(76, 92, 140),
	             rgb(40, 52, 96), rgb(20, 20, 32), rgb(132, 132, 136), rgb(144, 144, 108), rgb(40, 48, 20),
	             rgb(176, 176, 176)),
	    keyframe(1000, rgb(164, 164, 160), black, rgb(84, 84, 88), rgb(40, 44, 40), rgb(76, 92, 140), rgb(76, 92, 140),
	             rgb(56, 76, 136), rgb(20, 20, 32), rgb(160, 160, 164), rgb(236, 236, 112), rgb(6, 8, 4),
	             rgb(172, 168, 168)),
	    keyframe(1200, rgb(170, 170, 167), black, rgb(88, 88, 92), rgb(44, 44, 44), rgb(80, 96, 140), rgb(80, 96, 140),
	             rgb(60, 80, 140), rgb(20, 20, 32), rgb(150, 160, 150), rgb(140, 140, 140), rgb(20, 24, 10),
	             rgb(150, 160, 170)),
	    keyframe(1400, rgb(165, 165, 162), black, rgb(88, 88, 92), rgb(44, 44, 44), rgb(80, 96, 140), rgb(80, 96, 140),
	             rgb(60, 80, 140), rgb(20, 20, 32), rgb(150, 160, 150), rgb(140, 140, 140), rgb(20, 24, 10),
	             rgb(150, 160, 170)),
	    keyframe(1800, rgb(140, 100, 64), black, rgb(120, 96, 80), rgb(48, 40, 32), rgb(120, 88, 60), rgb(120, 88, 60),
	             rgb(100, 60, 40), rgb(40, 28, 20), rgb(220, 160, 90), rgb(120, 80, 60), rgb(60, 40, 20),
	             rgb(200, 150, 100)),
	    keyframe(2000, rgb(140, 100, 64), rgb(48, 48, 48), rgb(40, 40, 56), rgb(24, 24, 32), rgb(28, 28, 40),
	             rgb(28, 28, 40), rgb(20, 20, 36), rgb(40, 40, 60), rgb(60, 40, 30), rgb(30, 30, 40), rgb(40, 40, 40),
	             rgb(60, 60, 70)),
	    keyframe(2359, black, night_moon, night_sky, rgb(16, 28, 44), night_fog, night_fog, rgb(0, 1, 2),
	             rgb(64, 64, 84), rgb(2, 4, 2), rgb(14, 14, 20), rgb(52, 52, 52), rgb(44, 50, 60)),
	};
	return cfg;
}

// save_env's bytes for the synthetic day, re-parsed once to prove the writer's
// output is its own fixed point.
bool build_synth_env(std::vector<uint8_t> &bytes, std::string &err) {
	std::ostringstream out;
	if (!opennova::env::save_env(out, make_synth_env(), err)) return false;
	const std::string text = out.str();
	std::istringstream in(text);
	opennova::env::Config back;
	if (!opennova::env::load_env(in, back, err)) return false;
	std::ostringstream again;
	if (!opennova::env::save_env(again, back, err)) return false;
	if (again.str() != text) {
		err = "save_env(load_env(save_env(cfg))) differs from save_env(cfg)";
		return false;
	}
	if (back.keyframes.size() != 10 || back.name != "Synth_Full" || back.water_height_set) {
		err = "the re-parsed synthetic env lost a pinned field";
		return false;
	}
	bytes.assign(text.begin(), text.end());
	return true;
}

using test_io::read_file;

// 0 = byte-identical (or written), 1 = mismatch/missing.
int guard(const std::string &path, const std::vector<uint8_t> &bytes, bool write_mode) {
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), ("committed file missing; run with --write: " + path).c_str())) return 1;
	static const char kLfsSentinel[] = "version https://git-lfs";
	if (committed.size() >= sizeof(kLfsSentinel) - 1 &&
	    std::memcmp(committed.data(), kLfsSentinel, sizeof(kLfsSentinel) - 1) == 0) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	return expect(committed == bytes, ("differs from the generator output; regenerate with --write: " + path).c_str()) ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/env";
	int failures = 0;
	std::string err;

	std::vector<uint8_t> cloud_bytes;
	failures += !expect(opennova::encode_pcx_indexed(make_cloud_map(), cloud_bytes, err), ("encode_pcx_indexed: " + err).c_str());
	IndexedImage8 cloud_back;
	failures += !expect(opennova::decode_pcx_indexed(cloud_bytes.data(), cloud_bytes.size(), cloud_back, err) &&
	                            cloud_back.width == kSize && cloud_back.height == kSize &&
	                            cloud_back.indices == make_cloud_map().indices,
	                    "cloud01.pcx decodes back to the minted field");
	failures += guard(dir + "/cloud01.pcx", cloud_bytes, write_mode);

	std::vector<uint8_t> modulation_bytes;
	failures += !expect(opennova::encode_pcx_indexed(make_modulation_map(), modulation_bytes, err), ("encode_pcx_indexed (b): " + err).c_str());
	IndexedImage8 modulation_back;
	failures += !expect(opennova::decode_pcx_indexed(modulation_bytes.data(), modulation_bytes.size(), modulation_back, err) &&
	                            modulation_back.indices[0] == 128,
	                    "cloud01b.pcx decodes with its 128 center");
	failures += guard(dir + "/cloud01b.pcx", modulation_bytes, write_mode);

	std::vector<uint8_t> env_bytes;
	if (expect(build_synth_env(env_bytes, err), ("synth_full.env: " + err).c_str()))
		failures += guard(dir + "/synth_full.env", env_bytes, write_mode);
	else
		++failures;

	if (failures == 0 && !write_mode) std::printf("OK: fixtures/env synth_full.env + cloud maps byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
