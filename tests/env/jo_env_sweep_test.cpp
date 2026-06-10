// Full-install .env sweep: mounts a real Joint Operations install through the
// engine-faithful VFS, parses every .env it can see, and checks save->reparse
// semantic stability. Also prints field distributions that back the tracked
// divergence decisions in docs/env/env-tod-re.md:
//   - envscale (when present) precedes every *_rgb line (divergence #8)
//   - tod blocks that author fog_rgb also author skyfog_rgb, or rely on the
//     mirror (divergence #9)
//   - no shipped file exceeds the engine's 16-keyframe cap (divergence #3)
//
// Gated on OPENNOVA_JO_DIR so CI and fixture-only runs skip it:
//   OPENNOVA_JO_DIR="C:\...\Joint Operations Combined Arms" ctest -R env_jo_install
#include <env/env.h>
#include <vfs/vfs.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool rgb_equal(const opennova::env::Rgb &a, const opennova::env::Rgb &b) {
	const float eps = 0.5f / 255.0f;
	return std::fabs(a.r - b.r) <= eps && std::fabs(a.g - b.g) <= eps && std::fabs(a.b - b.b) <= eps;
}

bool keyframe_equal(const opennova::env::Keyframe &a, const opennova::env::Keyframe &b) {
	return a.time == b.time && rgb_equal(a.sun, b.sun) && rgb_equal(a.ground, b.ground) &&
	       rgb_equal(a.fog, b.fog) && rgb_equal(a.sky, b.sky) && rgb_equal(a.moon, b.moon) &&
	       rgb_equal(a.skyfog, b.skyfog) && rgb_equal(a.skybase, b.skybase) &&
	       rgb_equal(a.skybright, b.skybright) && rgb_equal(a.skyhighlight, b.skyhighlight) &&
	       rgb_equal(a.cloudbase, b.cloudbase) && rgb_equal(a.cloudhighlight, b.cloudhighlight) &&
	       rgb_equal(a.cloudedge, b.cloudedge);
}

// Streaming scan for divergence-assumption checks on the raw text.
struct TextFacts {
	bool envscale_after_color = false;
	int tod_blocks = 0;
	int fog_without_skyfog = 0;
};

TextFacts scan_text(const std::string &text) {
	TextFacts facts;
	std::istringstream input(text);
	std::string line;
	bool seen_color = false;
	bool in_tod = false;
	bool block_has_fog = false;
	bool block_has_skyfog = false;
	while (std::getline(input, line)) {
		const size_t comment = line.find(';');
		if (comment != std::string::npos) {
			line = line.substr(0, comment);
		}
		std::istringstream tokens(line);
		std::string key;
		tokens >> key;
		for (auto &c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (key.empty()) continue;
		if (key == "envscale" && seen_color) facts.envscale_after_color = true;
		if (key.size() > 4 && key.compare(key.size() - 4, 4, "_rgb") == 0) seen_color = true;
		if (key == "tod_begin") {
			++facts.tod_blocks;
			in_tod = true;
			block_has_fog = false;
			block_has_skyfog = false;
		} else if (key == "tod_end") {
			if (in_tod && block_has_fog && !block_has_skyfog) ++facts.fog_without_skyfog;
			in_tod = false;
		} else if (in_tod && key == "fog_rgb") {
			block_has_fog = true;
		} else if (in_tod && key == "skyfog_rgb") {
			block_has_skyfog = true;
		}
	}
	return facts;
}

} // namespace

int main() {
	const char *dir = std::getenv("OPENNOVA_JO_DIR");
	if (!dir || !*dir) {
		std::printf("SKIP: set OPENNOVA_JO_DIR to a JO install to run the full .env sweep\n");
		return 0;
	}

	opennova::Vfs vfs;
	if (!vfs.mount_game(dir, std::string(), opennova::VfsMountMode::Packed)) {
		std::fprintf(stderr, "FAIL: mount_game(%s): %s\n", dir, vfs.last_error().c_str());
		return 1;
	}

	int checked = 0;
	int failures = 0;
	int with_water_height = 0;
	int envscale_after_color_files = 0;
	int fog_without_skyfog_blocks = 0;
	int over_cap_files = 0;
	std::map<int, int> fog_types;
	std::map<int, int> advanced_clouds;

	for (const auto &loc : vfs.list_files()) {
		const std::string &name = loc.logical_name;
		if (name.size() < 4) continue;
		std::string ext = name.substr(name.size() - 4);
		for (auto &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (ext != ".env") continue;

		std::vector<uint8_t> bytes;
		if (!vfs.read_file_raw(name, bytes) || bytes.empty()) continue;
		const std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());

		++checked;
		opennova::env::Config parsed;
		std::string error;
		std::istringstream input(text);
		if (!opennova::env::load_env(input, parsed, error)) {
			std::fprintf(stderr, "FAIL: %s: parse: %s\n", name.c_str(), error.c_str());
			++failures;
			continue;
		}

		std::ostringstream saved;
		if (!opennova::env::save_env(saved, parsed, error)) {
			std::fprintf(stderr, "FAIL: %s: save: %s\n", name.c_str(), error.c_str());
			++failures;
			continue;
		}
		opennova::env::Config reparsed;
		std::istringstream saved_input(saved.str());
		if (!opennova::env::load_env(saved_input, reparsed, error)) {
			std::fprintf(stderr, "FAIL: %s: reparse: %s\n", name.c_str(), error.c_str());
			++failures;
			continue;
		}

		bool stable = reparsed.keyframes.size() == parsed.keyframes.size() &&
		              reparsed.curtime == parsed.curtime && reparsed.fog_type == parsed.fog_type &&
		              std::fabs(reparsed.fog_level - parsed.fog_level) <= 0.5f &&
		              std::fabs(reparsed.envscale - parsed.envscale) <= 0.001f &&
		              reparsed.water_height_set == parsed.water_height_set &&
		              rgb_equal(reparsed.water_rgb, parsed.water_rgb);
		if (stable) {
			for (size_t i = 0; i < parsed.keyframes.size(); ++i) {
				if (!keyframe_equal(parsed.keyframes[i], reparsed.keyframes[i])) {
					stable = false;
					break;
				}
			}
		}
		if (!stable) {
			std::fprintf(stderr, "FAIL: %s: save->reparse not semantically stable\n", name.c_str());
			++failures;
			continue;
		}

		const TextFacts facts = scan_text(text);
		if (facts.envscale_after_color) ++envscale_after_color_files;
		fog_without_skyfog_blocks += facts.fog_without_skyfog;
		if (facts.tod_blocks > opennova::env::kMaxTodKeyframes) ++over_cap_files;
		if (parsed.water_height_set) ++with_water_height;
		++fog_types[parsed.fog_type];
		++advanced_clouds[parsed.advanced_clouds];
	}

	std::printf("env sweep: %d files, %d with water_height, %d over the 16-keyframe cap\n",
	            checked, with_water_height, over_cap_files);
	std::printf("           %d files set envscale after a color, %d tod blocks have fog without skyfog\n",
	            envscale_after_color_files, fog_without_skyfog_blocks);
	for (const auto &entry : fog_types) {
		std::printf("           fog_type %d: %d files\n", entry.first, entry.second);
	}
	for (const auto &entry : advanced_clouds) {
		std::printf("           advanced_clouds %d: %d files\n", entry.first, entry.second);
	}

	if (checked == 0) {
		std::fprintf(stderr, "FAIL: no .env files found in the install\n");
		return 1;
	}
	if (failures) {
		std::fprintf(stderr, "FAIL: %d of %d .env files failed the sweep\n", failures, checked);
		return 1;
	}
	if (envscale_after_color_files > 0) {
		std::fprintf(stderr, "FAIL: envscale-after-color found; divergence #8 assumption violated\n");
		return 1;
	}
	std::printf("OK: %d .env files parse and round-trip semantically\n", checked);
	return 0;
}
